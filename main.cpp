// =============================================================================
//  통합 관제 대시보드 - C++ 백엔드 웹 서버 (main.cpp)
//
//  index.html(프론트엔드)와 연동되는 오프라인(폐쇄망) 전용 HTTP 서버.
//  외부 종속성이 없는 헤더 온리 라이브러리 2개만 사용한다:
//    - third_party/httplib.h  (cpp-httplib)
//    - third_party/json.hpp   (nlohmann/json)
//
//  빌드 (인터넷/패키지 매니저 불필요):
//    g++ -std=c++17 -O2 -pthread main.cpp -o server
//  실행:
//    ./server            # 기본 포트 8080, 현재 폴더의 Index.html 서빙
//
//  프론트엔드는 상대경로(/api/events, /api/dashboard)로 호출하므로 이 서버가
//  정적 파일(Index.html)까지 같은 오리진에서 함께 서빙한다.
// =============================================================================

#include "third_party/httplib.h"
#include "third_party/json.hpp"

#include <algorithm>
#include <chrono>
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
// 심각도 레벨 정의 (낮음 -> 높음). 상향(Escalation) 시 인덱스를 1 증가시킨다.
// -----------------------------------------------------------------------------
enum Severity { NORMAL = 0, MINOR = 1, MAJOR = 2, CRITICAL = 3, CATASTROPHIC = 4 };

static const char* severityName(int idx) {
    switch (idx) {
        case NORMAL:       return "Normal";
        case MINOR:        return "Minor";
        case MAJOR:        return "Major";
        case CRITICAL:     return "Critical";
        case CATASTROPHIC: return "Catastrophic";
        default:           return "Normal";
    }
}

// [초기 심각도] 레벨/이벤트 종류 기반 기본 판단
static int baseSeverity(const std::string& level, const std::string& type) {
    if (level == "ERROR") {
        if (type == "네트워크 단절" || type == "프로세스 다운") return CATASTROPHIC;
        return CRITICAL;
    }
    if (level == "WARN") {
        if (type == "리소스 임계치 초과") return MAJOR;
        return MINOR;
    }
    // INFO / DEBUG / 그 외
    return NORMAL;
}

// -----------------------------------------------------------------------------
// 알람 레코드: 최초 발생 시각과 발생 시각 목록(occurrence_times)을 관리한다.
// -----------------------------------------------------------------------------
struct AlarmRecord {
    std::string module;
    std::string level;
    std::string type;
    int         count = 0;
    int         severity = NORMAL;           // 최종(상향 반영) 심각도
    time_point  first_time;                  // 최초 발생 시각 (제거되지 않음)
    time_point  last_time;                   // 최근 발생 시각
    std::vector<time_point> occurrence_times; // 발생 시각 목록 (10분 슬라이딩 윈도우)
};

// 전역 저장소: Key = moduleId + "_" + eventType
static std::unordered_map<std::string, AlarmRecord> g_alarms;
static std::mutex g_mtx;

// 판별 기준 시간(초). 명세상 10분(600초)이 기본값이며, 테스트 편의를 위해
// 환경변수 ALARM_WINDOW_SECONDS 로만 재정의할 수 있다(미설정 시 정확히 600초).
static long windowSeconds() {
    static long cached = [] {
        const char* env = std::getenv("ALARM_WINDOW_SECONDS");
        if (env) {
            try { long v = std::stol(env); if (v > 0) return v; } catch (...) {}
        }
        return 600L; // 10분
    }();
    return cached;
}

// time_point -> "YYYY-MM-DD HH:MM:SS" (로컬 타임)
static std::string toTimeString(const time_point& tp) {
    std::time_t t = clock_t_::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);   // MSVC (인자 순서가 POSIX와 반대)
#else
    localtime_r(&t, &tm_buf);   // POSIX
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

// Index.html 파일 내용을 읽어 반환 (없으면 빈 문자열)
static std::string readIndexHtml() {
    const char* env = std::getenv("INDEX_FILE");
    std::string path = env ? env : "Index.html";
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return std::string();
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

// -----------------------------------------------------------------------------
// POST /api/events 처리: 수신 -> 중복제거 -> 심각도 판단/상향
// -----------------------------------------------------------------------------
static void handlePostEvent(const std::string& module,
                            const std::string& level,
                            const std::string& type) {
    const time_point now = clock_t_::now();
    const std::string key = module + "_" + type;
    const std::chrono::seconds window(windowSeconds());

    std::lock_guard<std::mutex> lock(g_mtx);
    auto it = g_alarms.find(key);
    if (it == g_alarms.end()) {
        // [신규 등록]
        AlarmRecord rec;
        rec.module = module;
        rec.level  = level;
        rec.type   = type;
        rec.count  = 1;
        rec.first_time = now;
        rec.last_time  = now;
        rec.occurrence_times.push_back(now);
        it = g_alarms.emplace(key, std::move(rec)).first;
    } else {
        // [중복 제거] 누적 건수 +1, 발생 시각 추가
        AlarmRecord& rec = it->second;
        rec.level = level; // 동일 종류 내 최신 레벨 반영
        rec.count += 1;
        rec.last_time = now;
        rec.occurrence_times.push_back(now);
    }

    AlarmRecord& rec = it->second;

    // [윈도우 정리] 10분(600초)이 지난 오래된 타임스탬프 제거
    const time_point cutoff = now - window;
    auto& times = rec.occurrence_times;
    times.erase(std::remove_if(times.begin(), times.end(),
                               [&](const time_point& t) { return t < cutoff; }),
                times.end());

    // [심각도 판단] 기본 심각도 재계산 후 상향 조건 검사
    int sev = baseSeverity(rec.level, rec.type);

    // 조건 1: 최근 10분 이내 발생 건수가 3회 이상
    const bool cond_repeat = static_cast<int>(times.size()) >= 3;
    // 조건 2: 최초 발생 시각과의 차이가 10분(600초) 이상
    const bool cond_duration =
        std::chrono::duration_cast<std::chrono::seconds>(now - rec.first_time) >= window;

    if (cond_repeat || cond_duration) {
        sev = std::min(sev + 1, static_cast<int>(CATASTROPHIC)); // 1단계 상향(Catastrophic 초과 불가)
    }
    rec.severity = sev;
}

// -----------------------------------------------------------------------------
// GET /api/dashboard 직렬화: 최근 발생 순으로 정렬해 JSON 배열 반환
// -----------------------------------------------------------------------------
static json buildDashboard() {
    std::lock_guard<std::mutex> lock(g_mtx);

    std::vector<const AlarmRecord*> rows;
    rows.reserve(g_alarms.size());
    for (const auto& kv : g_alarms) rows.push_back(&kv.second);

    // 최근에 이벤트가 발생한 알람이 최상단에 오도록 정렬 (last_time 내림차순)
    std::sort(rows.begin(), rows.end(),
              [](const AlarmRecord* a, const AlarmRecord* b) {
                  return a->last_time > b->last_time;
              });

    json arr = json::array();
    for (const AlarmRecord* r : rows) {
        std::ostringstream msg;
        msg << "[" << r->module << "] " << r->type << " 발생 "
            << "(레벨: " << r->level << ", 누적 " << r->count << "건)";
        arr.push_back({
            {"firstTime", toTimeString(r->first_time)},
            {"lastTime",  toTimeString(r->last_time)},
            {"module",    r->module},
            {"level",     r->level},
            {"type",      r->type},
            {"severity",  severityName(r->severity)},
            {"count",     r->count},
            {"message",   msg.str()},
        });
    }
    return arr;
}

int main(int argc, char** argv) {
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
            res.set_content("Index.html not found", "text/plain; charset=utf-8");
        } else {
            res.set_content(html, "text/html; charset=utf-8");
        }
    };
    svr.Get("/", serveIndex);
    svr.Get("/Index.html", serveIndex);
    svr.Get("/index.html", serveIndex);

    // POST /api/events
    svr.Post("/api/events", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string module = body.value("module", "");
            std::string level  = body.value("level", "");
            std::string type   = body.value("type", "");
            if (module.empty() || level.empty() || type.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"module, level, type 필수"})",
                                "application/json; charset=utf-8");
                return;
            }
            handlePostEvent(module, level, type);
            res.set_content(R"({"result":"ok"})", "application/json; charset=utf-8");
        } catch (const std::exception& e) {
            res.status = 400;
            json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json; charset=utf-8");
        }
    });

    // GET /api/dashboard
    svr.Get("/api/dashboard", [](const httplib::Request&, httplib::Response& res) {
        json arr = buildDashboard();
        res.set_content(arr.dump(), "application/json; charset=utf-8");
    });

    printf("통합 관제 백엔드 서버 시작: http://localhost:%d  (판별 윈도우 %lds)\n",
           port, windowSeconds());
    fflush(stdout);

    if (!svr.listen("0.0.0.0", port)) {
        fprintf(stderr, "서버 시작 실패 (포트 %d 사용 중일 수 있음)\n", port);
        return 1;
    }
    return 0;
}
