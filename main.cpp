// =============================================================================
//  통합 관제 대시보드 - C++ 백엔드 (main.cpp)  [AMCM 엔터프라이즈 표준]
//
//  index.html(프론트엔드)와 연동되는 오프라인(폐쇄망) 전용 HTTP 서버.
//  프론트엔드는 시각화만 담당하고, 심각도 판단/상태 관리/페어링/시간 기반 상향 등
//  모든 비즈니스 로직은 이 백엔드가 전담한다.
//  헤더 온리 라이브러리 2개만 사용: third_party/httplib.h, third_party/json.hpp
//
//  빌드: g++ -std=c++17 -O2 -pthread main.cpp -o server   (또는 CMake / Makefile)
//  실행: ./server            # 기본 포트 8080, 현재 폴더의 index.html 서빙
//
//  API
//    POST /api/events                   - 이벤트 수신 {module,type,level,message}
//    GET  /api/dashboard                - 활성 알람(Open/In Progress) 배열 반환(8필드)
//    PUT  /api/events/{eventId}/status   - 상태 변경 {status}; Closed 시 맵에서 제거
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
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
using clock_t_ = std::chrono::system_clock;
using time_point = clock_t_::time_point;

// -----------------------------------------------------------------------------
// [상단 const 상수] 상향 기준. 운영 기본값을 두되, 테스트 시 환경변수로만 재정의
// 가능하게 하여 실제 코드 경로를 그대로 검증한다.
// -----------------------------------------------------------------------------
static long envLong(const char* name, long def) {
    if (const char* e = std::getenv(name)) {
        try { long v = std::stol(e); if (v >= 0) return v; } catch (...) {}
    }
    return def;
}
static const long COUNT_ESCALATION_THRESHOLD = envLong("COUNT_ESCALATION_THRESHOLD", 3); // 누적 3회 -> 1단계 상향
static const long ESCALATION_TIMEOUT_SEC = envLong("ESCALATION_TIMEOUT_SEC", 600);        // 지속 10분 -> 1단계 상향
static const long TIMER_SCAN_SEC         = envLong("TIMER_SCAN_SEC", 5);                   // 타임아웃 감시 주기

// -----------------------------------------------------------------------------
// 도메인 정의: 5대 이벤트 페어링(장애 <-> 복구)
// -----------------------------------------------------------------------------
static const char* T_NET_DISCONNECT = "네트워크 단절";     // ERROR
static const char* T_PROCESS_DOWN   = "프로세스 다운";     // ERROR
static const char* T_RESOURCE_OVER  = "리소스 임계치 초과"; // WARN
static const char* T_DATA_DELAY     = "데이터 처리 지연";   // WARN
static const char* T_HW_FAULT       = "하드웨어 오류";     // ERROR

static const char* T_NET_RECOVER    = "네트워크 복구";     // INFO
static const char* T_PROCESS_OK     = "프로세스 정상화";   // INFO
static const char* T_RESOURCE_OK    = "리소스 정상";       // INFO
static const char* T_DATA_OK        = "처리 정상화";       // INFO
static const char* T_HW_OK          = "하드웨어 정상";     // INFO

// 복구(정상) 유형 -> 대응 장애 유형. 매칭 없으면 빈 문자열.
static std::string pairedFaultType(const std::string& recoveryType) {
    if (recoveryType == T_NET_RECOVER) return T_NET_DISCONNECT;
    if (recoveryType == T_PROCESS_OK)  return T_PROCESS_DOWN;
    if (recoveryType == T_RESOURCE_OK) return T_RESOURCE_OVER;
    if (recoveryType == T_DATA_OK)     return T_DATA_DELAY;
    if (recoveryType == T_HW_OK)       return T_HW_FAULT;
    return "";
}
static bool isRecoveryType(const std::string& type) {
    return !pairedFaultType(type).empty();
}
static bool isErrorLevel(const std::string& level) {
    return level == "ERROR" || level == "WARN";
}
// eventId 생성용 유형 코드 (예: AMCM-NET-001)
static std::string typeCode(const std::string& type) {
    if (type == T_NET_DISCONNECT || type == T_NET_RECOVER) return "NET";
    if (type == T_PROCESS_DOWN   || type == T_PROCESS_OK)  return "PRC";
    if (type == T_RESOURCE_OVER  || type == T_RESOURCE_OK) return "RES";
    if (type == T_DATA_DELAY     || type == T_DATA_OK)     return "DAT";
    if (type == T_HW_FAULT       || type == T_HW_OK)       return "HW";
    return "GEN";
}

// 심각도 5단계 (낮음 -> 높음). 상향 시 한 단계 위로, Catastrophic 이 최대치.
static std::string bumpSeverity(const std::string& s) {
    if (s == "Normal")   return "Minor";
    if (s == "Minor")    return "Major";
    if (s == "Major")    return "Critical";
    if (s == "Critical") return "Catastrophic";
    return "Catastrophic";
}

// [초기 가중치 차등화] 신규 장애의 초기 severity (유형 기반)
static std::string initialSeverity(const std::string& type) {
    if (type == T_NET_DISCONNECT || type == T_PROCESS_DOWN || type == T_HW_FAULT) return "Major";
    if (type == T_RESOURCE_OVER  || type == T_DATA_DELAY) return "Minor";
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
    bool        escalated_by_count   = false; // 카운트 상향 1회성 플래그
    bool        escalated_by_timeout = false; // 타임아웃 상향 1회성 플래그
};

// 활성 알람 맵: Active Key = module + "|" + type. 요청 처리 스레드 + 타이머 스레드가
// 함께 접근하므로 반드시 g_mtx 로 보호한다.
static std::unordered_map<std::string, Alarm> g_active;
static std::mutex g_mtx;
static std::atomic<long> g_seq{0};

static std::string activeKey(const std::string& module, const std::string& type) {
    return module + "|" + type;
}
// 고유 eventId 발급: "MODULE-TYPECODE-NNN" (예: AMCM-NET-001)
static std::string newEventId(const std::string& module, const std::string& type) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s-%s-%03ld",
                  module.c_str(), typeCode(type).c_str(), static_cast<long>(++g_seq));
    return std::string(buf);
}

// time_point -> "YYYY-MM-DD HH:MM:SS" (로컬 타임, 크로스 플랫폼)
static std::string toTimeString(const time_point& tp) {
    std::time_t t = clock_t_::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}
static long secondsSince(const time_point& from, const time_point& to) {
    return std::chrono::duration_cast<std::chrono::seconds>(to - from).count();
}

// -----------------------------------------------------------------------------
// POST /api/events : 라우팅(복구 페어링 / 신규 / 상향)
// -----------------------------------------------------------------------------
static void handlePostEvent(const std::string& module,
                            const std::string& type,
                            const std::string& level,
                            const std::string& message) {
    const time_point now = clock_t_::now();
    const std::string msg = !message.empty()
        ? message
        : ("[" + module + "] " + type + " (" + level + ")");

    std::lock_guard<std::mutex> lock(g_mtx);

    // ===== [복구 페어링] INFO 복구 이벤트 =====
    // 동일 module 의 짝꿍 장애를 Closed 처리(맵에서 즉시 제거)하고, 복구 이벤트 자체를
    // severity "Normal" 신규 알람으로 등록한다.
    if (level == "INFO" && isRecoveryType(type)) {
        const std::string faultType = pairedFaultType(type);
        auto fit = g_active.find(activeKey(module, faultType));
        if (fit != g_active.end() && isErrorLevel(fit->second.level) &&
            (fit->second.status == "Open" || fit->second.status == "In Progress")) {
            g_active.erase(fit);  // 짝꿍 장애 Closed -> 즉시 제거(메모리 회수)
        }
        Alarm rec;
        rec.eventId    = newEventId(module, type);
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

    // ===== [신규 장애] Active Map 에 없을 때 =====
    const std::string key = activeKey(module, type);
    auto it = g_active.find(key);
    if (it == g_active.end()) {
        Alarm rec;
        rec.eventId    = newEventId(module, type);
        rec.module     = module;
        rec.type       = type;
        rec.level      = level;
        rec.severity   = initialSeverity(type);   // 초기 가중치 차등화
        rec.status     = "Open";
        rec.count      = 1;
        rec.first_time = now;
        rec.last_time  = now;
        rec.message    = msg;
        g_active.emplace(key, std::move(rec));
        return;
    }

    // ===== [기존 장애 재수신] count++ 및 카운트 상향 =====
    Alarm& a = it->second;
    a.count += 1;
    a.last_time = now;
    a.message = msg;
    // count 가 임계치(기본 3) 이상이면 severity 를 한 단계 상향(1회성)
    if (!a.escalated_by_count && a.count >= COUNT_ESCALATION_THRESHOLD) {
        a.severity = bumpSeverity(a.severity);
        a.escalated_by_count = true;
    }
}

// -----------------------------------------------------------------------------
// [주기 타임아웃 상향 스레드] Active Map 등록 후 ESCALATION_TIMEOUT_SEC(600s) 이상
// Closed 되지 않은 이벤트의 severity 를 1단계 상향(이벤트당 1회). Mutex 보호.
// -----------------------------------------------------------------------------
static std::atomic<bool> g_stop_timer{false};
static void timerLoop() {
    while (!g_stop_timer.load()) {
        for (long i = 0; i < TIMER_SCAN_SEC * 10 && !g_stop_timer.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (g_stop_timer.load()) break;

        const time_point now = clock_t_::now();
        std::lock_guard<std::mutex> lock(g_mtx);
        for (auto& kv : g_active) {
            Alarm& a = kv.second;
            if (!a.escalated_by_timeout &&
                secondsSince(a.first_time, now) >= ESCALATION_TIMEOUT_SEC) {
                a.severity = bumpSeverity(a.severity);
                a.escalated_by_timeout = true;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// GET /api/dashboard : 활성 알람(Open/In Progress)만, 최근 발생 순 정렬.
// 프론트엔드 필수 8필드(lastTime, module, type, severity, status, count, message,
// eventId)를 포함한다(추가로 level, firstTime 도 제공).
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

// PUT /api/events/{eventId}/status : Closed 는 맵에서 즉시 제거. true=처리, false=미존재.
static bool updateStatus(const std::string& eventId, const std::string& status) {
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto it = g_active.begin(); it != g_active.end(); ++it) {
        if (it->second.eventId == eventId) {
            if (status == "Closed") g_active.erase(it);
            else                    it->second.status = status;
            return true;
        }
    }
    return false;
}

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
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    int port = 8080;
    if (argc > 1) {
        try { port = std::stoi(argv[1]); } catch (...) {}
    }

    httplib::Server svr;

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

    // POST /api/events  (수신: module, type, level, message)
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
            if (status != "Open" && status != "In Progress" && status != "Closed") {
                res.status = 400;
                res.set_content(R"({"error":"status 는 Open/In Progress/Closed 중 하나여야 함"})",
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

    // 종료 시그널 + 타임아웃 상향 스레드
    g_server = &svr;
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::thread timer(timerLoop);

    printf("통합 관제 백엔드(AMCM 표준) 시작: http://localhost:%d\n", port);
    printf("  카운트상향 >=%ld | 타임아웃상향 %lds | 감시주기 %lds\n",
           COUNT_ESCALATION_THRESHOLD, ESCALATION_TIMEOUT_SEC, TIMER_SCAN_SEC);
    printf("종료하려면 Ctrl+C 를 누르세요.\n");
    fflush(stdout);

    const bool ok = svr.listen("0.0.0.0", port);

    g_stop_timer.store(true);
    if (timer.joinable()) timer.join();

    if (!ok && !g_shutting_down.load()) {
        fprintf(stderr, "서버 시작 실패 (포트 %d 사용 중일 수 있음)\n", port);
        return 1;
    }
    printf("서버를 정상적으로 종료했습니다.\n");
    return 0;
}
