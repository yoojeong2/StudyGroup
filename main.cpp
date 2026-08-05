// =============================================================================
//  통합 관제 대시보드 - C++ 백엔드 웹 서버 (main.cpp) [엔터프라이즈 리팩토링]
//
//  index.html(프론트엔드)와 연동되는 오프라인(폐쇄망) 전용 HTTP 서버.
//  헤더 온리 라이브러리 2개만 사용: third_party/httplib.h, third_party/json.hpp
//
//  빌드 (인터넷/패키지 매니저 불필요):
//    g++ -std=c++17 -O2 -pthread main.cpp -o server   (또는 CMake / Makefile)
//  실행: ./server            # 기본 포트 8080, 현재 폴더의 index.html 서빙
//
//  [핵심 원칙] 프론트엔드 연산 배제: 심각도 판단/시간 계산/스로틀링/장애-복구
//  페어링 등 모든 로직을 이 백엔드가 전담한다.
//
//  API
//    POST /api/events                     - 이벤트 수신(수신 규격: module/type/level/message)
//    GET  /api/dashboard                  - 활성 알람(Open/In Progress) 배열 반환
//    PUT  /api/events/{eventId}/status     - 상태 변경(In Progress/Closed)
// =============================================================================

#include "third_party/httplib.h"
#include "third_party/json.hpp"

#if defined(_WIN32)
#include <windows.h>  // SetConsoleOutputCP (콘솔 UTF-8 출력용)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
using clock_t_ = std::chrono::system_clock;
using time_point = clock_t_::time_point;

// -----------------------------------------------------------------------------
// [상단 const 상수] 상향(Escalation)/스로틀링 기준. 운영 기본값을 두되, 테스트 시
// 환경변수로만 재정의할 수 있게 하여 실제 코드 경로를 그대로 검증할 수 있다.
// -----------------------------------------------------------------------------
static long envLong(const char* name, long def) {
    if (const char* e = std::getenv(name)) {
        try { long v = std::stol(e); if (v >= 0) return v; } catch (...) {}
    }
    return def;
}
static const long THROTTLE_MS            = envLong("THROTTLE_MS", 1000);          // 폭주 방어 1초
static const long ESC_NET_DISCONNECT_SEC = envLong("ESC_NET_DISCONNECT_SEC", 180); // 네트워크 단절 3분 -> Critical
static const long ESC_PROCESS_DOWN_SEC   = envLong("ESC_PROCESS_DOWN_SEC", 60);    // 프로세스 다운 1분 -> Critical
static const long ESC_PROCESS_DOWN_COUNT = envLong("ESC_PROCESS_DOWN_COUNT", 3);   // 프로세스 다운 누적 3건 -> Critical
static const long ESC_RESOURCE_SEC       = envLong("ESC_RESOURCE_SEC", 300);       // 리소스 임계치 초과 5분 -> Major

// 이벤트 종류 상수
static const char* T_NET_DISCONNECT = "네트워크 단절";
static const char* T_PROCESS_DOWN   = "프로세스 다운";
static const char* T_RESOURCE_OVER  = "리소스 임계치 초과";
static const char* T_DATA_DELAY     = "데이터 처리 지연";

static bool isFaultType(const std::string& type) {
    return type == T_NET_DISCONNECT || type == T_PROCESS_DOWN ||
           type == T_RESOURCE_OVER  || type == T_DATA_DELAY;
}
static bool isErrorLevel(const std::string& level) {
    return level == "ERROR" || level == "WARN";
}

// 심각도 랭크(하향 방지용): Normal < Minor < Major < Critical
static int sevRank(const std::string& s) {
    if (s == "Critical") return 3;
    if (s == "Major")    return 2;
    if (s == "Minor")    return 1;
    return 0; // Normal
}
static const std::string& maxSeverity(const std::string& a, const std::string& b) {
    return sevRank(a) >= sevRank(b) ? a : b;
}

// [초기 심각도 매핑] 신규 알람 등록 시
static std::string initialSeverity(const std::string& type, const std::string& level) {
    if (level == "ERROR" && (type == T_NET_DISCONNECT || type == T_PROCESS_DOWN)) return "Major";
    if (level == "WARN"  && type == T_RESOURCE_OVER) return "Minor";
    return "Normal";
}

// -----------------------------------------------------------------------------
// 활성 알람 레코드
// -----------------------------------------------------------------------------
struct Alarm {
    std::string eventId;
    std::string module;
    std::string type;
    std::string level;
    std::string severity;
    std::string status;   // "Open" / "In Progress" (Closed 는 즉시 맵에서 제거)
    int         count = 0;
    time_point  first_time;
    time_point  last_time;
    std::string message;
};

// 활성 알람 맵: Active Key = module + "|" + type
static std::unordered_map<std::string, Alarm> g_active;
static std::mutex g_mtx;
static std::atomic<long> g_seq{0};

static std::string activeKey(const std::string& module, const std::string& type) {
    return module + "|" + type;
}
static std::string newEventId() {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "EVT-%06ld", static_cast<long>(++g_seq));
    return std::string(buf);
}

// time_point -> "YYYY-MM-DD HH:MM:SS" (로컬 타임, 크로스 플랫폼)
static std::string toTimeString(const time_point& tp) {
    std::time_t t = clock_t_::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);   // MSVC
#else
    localtime_r(&t, &tm_buf);   // POSIX
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

static long secondsSince(const time_point& from, const time_point& to) {
    return std::chrono::duration_cast<std::chrono::seconds>(to - from).count();
}
static long millisSince(const time_point& from, const time_point& to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

// -----------------------------------------------------------------------------
// POST /api/events 처리: 라우팅(Case A/B) -> 스로틀링 -> 심각도 판단/상향
// -----------------------------------------------------------------------------
static void handlePostEvent(const std::string& module,
                            const std::string& type,
                            const std::string& level,
                            const std::string& message) {
    const time_point now = clock_t_::now();
    // 메시지가 비어 있으면(현 프론트엔드는 message 미전송) 기본 메시지 생성
    const std::string msg = !message.empty()
        ? message
        : ("[" + module + "] " + type + " (" + level + ")");

    std::lock_guard<std::mutex> lock(g_mtx);

    // ===== [Case A] 복구 이벤트(INFO) 수신 - 장애-복구 페어링 =====
    // level == INFO 이고 복구 유형(장애 종류)일 때: 동일 module 의 진행 중 에러 알람(짝꿍)을
    // 백엔드가 즉시 Closed 처리(맵에서 제거)하고, 복구 이벤트 자체를 새 활성 알람으로 등록.
    if (level == "INFO" && isFaultType(type)) {
        for (auto it = g_active.begin(); it != g_active.end(); ) {
            const Alarm& a = it->second;
            const bool pairedError = (a.module == module) && isErrorLevel(a.level) &&
                                     (a.status == "Open" || a.status == "In Progress");
            if (pairedError) {
                it = g_active.erase(it);   // Closed -> 활성 맵에서 즉시 제거(메모리 회수)
            } else {
                ++it;
            }
        }
        Alarm rec;
        rec.eventId    = newEventId();
        rec.module     = module;
        rec.type       = type;
        rec.level      = level;
        rec.severity   = "Normal";
        rec.status     = "Open";
        rec.count      = 1;
        rec.first_time = now;
        rec.last_time  = now;
        rec.message    = msg;
        g_active[activeKey(module, type)] = std::move(rec);
        return;
    }

    // ===== [Case B] 일반 이벤트(ERROR/WARN 등) 수신 =====
    const std::string key = activeKey(module, type);
    auto it = g_active.find(key);

    // 신규 발생 (활성 항목 없음)
    if (it == g_active.end()) {
        Alarm rec;
        rec.eventId    = newEventId();
        rec.module     = module;
        rec.type       = type;
        rec.level      = level;
        rec.severity   = initialSeverity(type, level);
        rec.status     = "Open";
        rec.count      = 1;
        rec.first_time = now;
        rec.last_time  = now;
        rec.message    = msg;
        g_active.emplace(key, std::move(rec));
        return;
    }

    // 기존 활성 항목 업데이트
    Alarm& a = it->second;
    const long gapMs = millisSince(a.last_time, now);
    a.count += 1;
    a.last_time = now;

    // [이벤트 폭주 방어(Throttling)] 동일 키가 1초 이내 폭주하면 count 만 올리고
    // 무거운 상향(Escalation)/메시지 처리는 스킵한다.
    if (gapMs < THROTTLE_MS) {
        return;
    }

    a.message = msg; // message 덮어쓰기

    // [시간/누적 기반 상향(Escalation)] - 하향은 하지 않음(max 사용)
    const long sustainedSec = secondsSince(a.first_time, now);
    std::string escalated = a.severity;
    if (a.type == T_NET_DISCONNECT) {
        if (sustainedSec >= ESC_NET_DISCONNECT_SEC) escalated = "Critical";
    } else if (a.type == T_PROCESS_DOWN) {
        if (sustainedSec >= ESC_PROCESS_DOWN_SEC || a.count >= ESC_PROCESS_DOWN_COUNT) escalated = "Critical";
    } else if (a.type == T_RESOURCE_OVER) {
        if (sustainedSec >= ESC_RESOURCE_SEC) escalated = "Major";
    }
    a.severity = maxSeverity(a.severity, escalated);
}

// -----------------------------------------------------------------------------
// GET /api/dashboard 직렬화: 활성 알람(Open/In Progress)만, 최근 발생 순 정렬
// -----------------------------------------------------------------------------
static json buildDashboard() {
    std::lock_guard<std::mutex> lock(g_mtx);

    std::vector<const Alarm*> rows;
    rows.reserve(g_active.size());
    for (const auto& kv : g_active) {
        const Alarm& a = kv.second;
        if (a.status == "Open" || a.status == "In Progress") rows.push_back(&a);
    }
    std::sort(rows.begin(), rows.end(),
              [](const Alarm* x, const Alarm* y) { return x->last_time > y->last_time; });

    json arr = json::array();
    for (const Alarm* a : rows) {
        arr.push_back({
            {"eventId",   a->eventId},
            {"module",    a->module},
            {"type",      a->type},
            {"level",     a->level},
            {"severity",  a->severity},
            {"status",    a->status},
            {"count",     a->count},
            {"firstTime", toTimeString(a->first_time)},
            {"lastTime",  toTimeString(a->last_time)},
            {"message",   a->message},
        });
    }
    return arr;
}

// PUT /api/events/{eventId}/status : 상태 변경. Closed 는 맵에서 즉시 제거.
// 반환: true = 처리됨, false = 해당 eventId 없음
static bool updateStatus(const std::string& eventId, const std::string& status) {
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto it = g_active.begin(); it != g_active.end(); ++it) {
        if (it->second.eventId == eventId) {
            if (status == "Closed") {
                g_active.erase(it);          // 메모리 누수 방지: 활성 맵에서 즉시 제거
            } else {
                it->second.status = status;  // "In Progress"
            }
            return true;
        }
    }
    return false;
}

// index.html 파일 내용을 읽어 반환 (없으면 빈 문자열)
static std::string readIndexHtml() {
    const char* env = std::getenv("INDEX_FILE");
    std::string path = env ? env : "index.html";
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return std::string();
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

// -----------------------------------------------------------------------------
// 우아한 종료(Graceful Shutdown)
// -----------------------------------------------------------------------------
static httplib::Server*  g_server = nullptr;
static std::atomic<bool> g_shutting_down{false};
static void handleSignal(int /*sig*/) {
    g_shutting_down.store(true);
    if (g_server) g_server->stop();
}

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);   // Windows 콘솔 한글 깨짐 방지
    SetConsoleCP(CP_UTF8);
#endif

    int port = 8080;
    if (argc > 1) {
        try { port = std::stoi(argv[1]); } catch (...) {}
    }

    httplib::Server svr;

    // 정적 프론트엔드 서빙 (같은 오리진 유지)
    auto serveIndex = [](const httplib::Request&, httplib::Response& res) {
        std::string html = readIndexHtml();
        if (html.empty()) {
            res.status = 404;
            res.set_content("index.html not found", "text/plain; charset=utf-8");
        } else {
            res.set_content(html, "text/html; charset=utf-8");
        }
    };
    svr.Get("/", serveIndex);
    svr.Get("/index.html", serveIndex);
    svr.Get("/Index.html", serveIndex);

    // POST /api/events
    svr.Post("/api/events", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string module  = body.value("module", "");
            std::string type    = body.value("type", "");
            std::string level   = body.value("level", "");
            std::string message = body.value("message", "");
            if (module.empty() || type.empty() || level.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"module, type, level 필수"})",
                                "application/json; charset=utf-8");
                return;
            }
            handlePostEvent(module, type, level, message);
            res.set_content(R"({"result":"ok"})", "application/json; charset=utf-8");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json; charset=utf-8");
        }
    });

    // GET /api/dashboard
    svr.Get("/api/dashboard", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildDashboard().dump(), "application/json; charset=utf-8");
    });

    // PUT /api/events/{eventId}/status
    svr.Put(R"(/api/events/([^/]+)/status)", [](const httplib::Request& req, httplib::Response& res) {
        try {
            const std::string eventId = req.matches[1];
            json body = json::parse(req.body);
            std::string status = body.value("status", "");
            if (status != "In Progress" && status != "Closed") {
                res.status = 400;
                res.set_content(R"({"error":"status 는 'In Progress' 또는 'Closed' 여야 함"})",
                                "application/json; charset=utf-8");
                return;
            }
            if (!updateStatus(eventId, status)) {
                res.status = 404;
                res.set_content(json({{"error", "eventId 없음"}, {"eventId", eventId}}).dump(),
                                "application/json; charset=utf-8");
                return;
            }
            res.set_content(json({{"result", "ok"}, {"eventId", eventId}, {"status", status}}).dump(),
                            "application/json; charset=utf-8");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json; charset=utf-8");
        }
    });

    // 종료 시그널 등록 (우아한 종료)
    g_server = &svr;
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    printf("통합 관제 백엔드 서버 시작: http://localhost:%d\n", port);
    printf("  스로틀=%ldms | 네트워크단절 %lds | 프로세스다운 %lds/누적%ld | 리소스 %lds -> Critical/Major\n",
           THROTTLE_MS, ESC_NET_DISCONNECT_SEC, ESC_PROCESS_DOWN_SEC, ESC_PROCESS_DOWN_COUNT, ESC_RESOURCE_SEC);
    printf("종료하려면 Ctrl+C 를 누르세요.\n");
    fflush(stdout);

    const bool ok = svr.listen("0.0.0.0", port);
    if (!ok && !g_shutting_down.load()) {
        fprintf(stderr, "서버 시작 실패 (포트 %d 사용 중일 수 있음)\n", port);
        return 1;
    }
    printf("서버를 정상적으로 종료했습니다.\n");
    return 0;
}
