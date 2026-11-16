#include "cache.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace agent_cpp {

// restituisce il tempo esatto
static std::string iso_timestamp() {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return ss.str();
}

static void write_json(const fs::path& path, const JsonObject& j) {
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        throw std::runtime_error("Impossibile scrivere " + path.string());
    out << j.dump(2, ' ', false, JsonObject::error_handler_t::replace);
    if (!out)
        throw std::runtime_error("Errore di scrittura su " + path.string());
}

static std::optional<JsonObject> read_json(const fs::path& path) {
    std::ifstream in(path);
    if (!in)
        return std::nullopt;
    try {
        return JsonObject::parse(in);
    } catch (const JsonObject::exception&) {
        return std::nullopt;
    }
}

// trasforma una stringa in un insieme di parole
static std::set<std::string> tokenize(const std::string& text) {
    static const std::string punct = ".,?!;:()[]\"'";
    std::set<std::string> tokens;
    std::string cur;
    auto flush = [&]() {
        std::size_t b = cur.find_first_not_of(punct);
        if (b != std::string::npos) {
            std::size_t e = cur.find_last_not_of(punct);
            tokens.insert(cur.substr(b, e - b + 1));
        }
        cur.clear();
    };
    for (unsigned char c : text) {
        if (std::isspace(c) || c == '_')
            flush();
        else
            cur.push_back(static_cast<char>(std::tolower(c))); // minuscolo
    }
    flush();
    return tokens;
}

// Similarità di Jaccard: |A ∩ B| / |A ∪ B| 
static double jaccard(const std::set<std::string>& a, const std::set<std::string>& b) {
    std::size_t inter = 0;
    for (const auto& t : a)
        if (b.count(t))
            ++inter;
    std::size_t uni = a.size() + b.size() - inter;
    return uni > 0 ? static_cast<double>(inter) / static_cast<double>(uni) : 0.0;
}

// costruttore
RunCache::RunCache(const std::string& cache_dir): cache_dir_(cache_dir), index_path_(cache_dir_ / "index.json") {}

JsonObject RunCache::load_index() const {
    auto idx = read_json(index_path_);
    if (idx && idx->is_object() && idx->contains("runs") && (*idx)["runs"].is_array())
        return *idx;
    return JsonObject{{"runs", JsonObject::array()}};
}

void RunCache::save_index(const JsonObject& index) const {
    write_json(index_path_, index);
}

void RunCache::save_run(const std::string& run_id, const std::string& prompt, const JsonObject& agent_config,
                        const StepResults& step_results, const JsonObject& final_result, const JsonObject& metadata) {
    const fs::path dir = cache_dir_ / run_id;
    fs::create_directories(dir);   // crea anche la cache_dir al primo salvataggio

    // Un file JSON per step, con tutti gli N risultati.
    for (const auto& [step_name, results] : step_results) {
        JsonObject arr = JsonObject::array();
        for (const auto& r : results)
            arr.push_back(r);
        write_json(dir / (step_name + ".json"), arr);
    }

    const std::string timestamp = iso_timestamp();
    write_json(dir / "metadata.json", JsonObject{
        {"run_id", run_id},
        {"prompt", prompt},
        {"timestamp", timestamp},
        {"agent_config", agent_config},
        {"final_result", final_result},
        {"metadata", metadata}
    });

    // Aggiorna l'indice evitando duplicati 
    JsonObject index = load_index();
    for (const auto& r : index["runs"])
        if (r.value("run_id", "") == run_id)
            return;
    index["runs"].push_back(JsonObject{
        {"run_id", run_id}, {"prompt", prompt}, {"timestamp", timestamp}
    });
    save_index(index);
}

std::vector<std::string> RunCache::find_similar_runs(const std::string& prompt, std::size_t top_k, double similarity_threshold) const {
    const auto prompt_words = tokenize(prompt);
    const JsonObject index = load_index();

    std::vector<std::pair<std::string, double>> sims;
    for (const auto& r : index["runs"])
        sims.emplace_back(r.value("run_id", ""), jaccard(prompt_words, tokenize(r.value("prompt", ""))));

    std::stable_sort(sims.begin(), sims.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<std::string> out;
    for (std::size_t i = 0; i < sims.size() && i < top_k; ++i)
        if (sims[i].second >= similarity_threshold && !sims[i].first.empty())
            out.push_back(sims[i].first);
    return out;
}

std::optional<std::vector<JsonObject>> RunCache::load_step_results(const std::string& run_id, const std::string& step_name) const {
    auto raw = read_json(cache_dir_ / run_id / (step_name + ".json"));
    if (!raw || !raw->is_array() || raw->empty())
        return std::nullopt;
    return std::vector<JsonObject>(raw->begin(), raw->end());
}

StepResults RunCache::load_all_step_results(const std::string& run_id) const {
    StepResults results;
    if (!fs::is_directory(cache_dir_ / run_id))
        return results;

    static const std::vector<std::string> step_names = {"decide_tool", "lookup_sales_data", "analyzing_data", "create_visualization"};
    for (const auto& step_name : step_names)
        if (auto r = load_step_results(run_id, step_name))
            results[step_name] = std::move(*r);
    return results;
}

}
