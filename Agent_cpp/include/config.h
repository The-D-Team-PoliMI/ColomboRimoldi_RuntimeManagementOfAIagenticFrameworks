#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <tuple>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <yaml-cpp/yaml.h>
#include "schema.h"

namespace agent_cpp {

/**
 * @brief Configurazione degli iperparametri per un singolo step (Best-of-N, LLM, CoT).
 * Modella fedelmente StepConfig del progetto Python.
 */
struct StepConfig {
    // Best-of-n sampling parameters
    int n = 1;
    std::string bon_param = "temperature"; // "temperature" | "top_p" | "top_k"
    double temp_min = 0.1;
    double temp_max = 0.1;

    // LLM generation parameters
    int max_tokens = 2000;
    double top_p_min = 1.0;
    double top_p_max = 1.0;
    std::optional<int> top_k_min = std::nullopt;
    std::optional<int> top_k_max = std::nullopt;
    std::optional<double> repeat_penalty = std::nullopt;
    std::optional<int> repeat_last_n = std::nullopt;
    int num_beams = 1;
    std::optional<int> no_repeat_ngram_size = std::nullopt;

    // Optional per-step LLM overrides
    std::optional<std::string> provider = std::nullopt;
    std::optional<std::string> model = std::nullopt;
    std::optional<std::string> ollama_url = std::nullopt;

    // Evaluation mode ("default" | "none")
    std::string eval_mode = "default";

    // Caching control
    bool use_cache = true;
    std::string cache_mode = "auto"; // "auto" | "skip" | "force_fresh"

    // CoT iterative refinement
    int cot_n = 1;

    // Metadata
    bool enabled = true;
    std::string step_name = "";

    // Generate (temperature, top_p, top_k) tuples matching Python's get_candidate_params()
    std::vector<std::tuple<double, double, std::optional<int>>> get_candidate_params() const {
        if (n <= 1) {
            return {{temp_min, top_p_min, top_k_min}};
        }

        std::vector<std::tuple<double, double, std::optional<int>>> params;
        params.reserve(static_cast<size_t>(n));

        if (bon_param == "top_p") {
            for (int i = 0; i < n; ++i) {
                double p = top_p_min + (top_p_max - top_p_min) * static_cast<double>(i) / static_cast<double>(n - 1);
                params.emplace_back(temp_min, p, top_k_min);
            }
            return params;
        }

        if (bon_param == "top_k") {
            int k_start = top_k_min.value_or(1);
            int k_end = top_k_max.value_or(k_start);
            for (int i = 0; i < n; ++i) {
                double k_val = static_cast<double>(k_start) +
                               (static_cast<double>(k_end - k_start) * static_cast<double>(i)) / static_cast<double>(n - 1);
                params.emplace_back(temp_min, top_p_min, static_cast<int>(k_val));
            }
            return params;
        }

        // Default: "temperature"
        for (int i = 0; i < n; ++i) {
            double t = temp_min + (temp_max - temp_min) * static_cast<double>(i) / static_cast<double>(n - 1);
            params.emplace_back(t, top_p_min, top_k_min);
        }
        return params;
    }

    std::vector<double> get_temperatures() const {
        std::vector<double> temps;
        for (const auto& [t, p, k] : get_candidate_params()) {
            temps.push_back(t);
        }
        return temps;
    }
};

/**
 * @brief Parametri di esecuzione del workflow estratti dalla sezione 'run' e 'tracing' del file YAML.
 */
struct RunParams {
    std::string prompt = "";
    std::optional<std::string> visualization_goal = std::nullopt;
    std::string agent_mode = "full"; // "full", "analysis", "lookup_only"
    bool lookup_only = false;
    bool no_vis = false;
    std::optional<std::string> run_id = std::nullopt;
    std::string save_dir = "./output";
    bool save_results = false;
    bool save_execution_artifacts = true;
    std::optional<std::string> reuse_from = std::nullopt;
    bool enable_codecarbon = false;
    bool interactive_config = false;

    // Tracing sub-dict
    bool tracing_enabled = false;
    std::optional<std::string> phoenix_endpoint = std::nullopt;
    std::optional<std::string> phoenix_api_key = std::nullopt;
    std::string project_name = "evaluating-agent";
};

/**
 * @brief Configurazione globale dell'agente con parametri per-step.
 */
struct AgentConfig {
    std::string provider = "google";
    std::string model = "gemini-3.5-flash-lite";
    std::string ollama_url = "http://127.0.0.1:11434/api";
    std::string gemini_api_key = "";
    std::string openai_api_key = "";
    std::string mode = "full"; // "full", "analysis", "lookup_only"
    std::string data_dir = "../data";

    std::unordered_map<std::string, StepConfig> steps = {
        {"decide_tool", StepConfig{
            .n = 1,
            .bon_param = "temperature",
            .temp_min = 0.15,
            .temp_max = 0.15,
            .max_tokens = 1000,
            .eval_mode = "none",
            .use_cache = false,
            .cot_n = 1,
            .step_name = "decide_tool"
        }},
        {"lookup_sales_data", StepConfig{
            .n = 1,
            .bon_param = "temperature",
            .temp_min = 0.15,
            .temp_max = 0.3,
            .max_tokens = 5000,
            .eval_mode = "default",
            .use_cache = true,
            .cot_n = 1,
            .step_name = "lookup_sales_data"
        }},
        {"analyzing_data", StepConfig{
            .n = 1,
            .bon_param = "temperature",
            .temp_min = 0.15,
            .temp_max = 0.7,
            .max_tokens = 7000,
            .eval_mode = "default",
            .use_cache = true,
            .cot_n = 1,
            .step_name = "analyzing_data"
        }},
        {"create_visualization", StepConfig{
            .n = 1,
            .bon_param = "temperature",
            .temp_min = 0.15,
            .temp_max = 0.5,
            .max_tokens = 7000,
            .eval_mode = "default",
            .use_cache = true,
            .cot_n = 1,
            .step_name = "create_visualization"
        }}
    };

    const StepConfig& get_step_config(const std::string& step_name) const {
        auto it = steps.find(step_name);
        if (it != steps.end()) {
            return it->second;
        }
        static const StepConfig default_cfg;
        return default_cfg;
    }

    StepConfig& get_mutable_step_config(const std::string& step_name) {
        return steps[step_name];
    }

    /**
     * @brief Carica la configurazione da un file YAML esattamente come in AgentConfig.from_yaml in Python.
     * @param yaml_path_str Percorso al file YAML (es. "config/run_config.yaml").
     * @return Tuple di (AgentConfig, RunParams, DatabaseSchema).
     */
    static std::tuple<AgentConfig, RunParams, DatabaseSchema> from_yaml(const std::string& yaml_path_str) {
        namespace fs = std::filesystem;
        fs::path yaml_path = fs::absolute(yaml_path_str);
        if (!fs::exists(yaml_path)) {
            throw std::runtime_error("Configuration file not found: " + yaml_path.string());
        }

        YAML::Node root = YAML::LoadFile(yaml_path.string());
        fs::path config_dir = yaml_path.parent_path();

        AgentConfig config;
        RunParams run_params;

        // Recupero fallback delle chiavi API dalle variabili d'ambiente
        if (const char* env = std::getenv("GEMINI_API_KEY")) {
            config.gemini_api_key = env;
        }
        if (const char* env = std::getenv("OPENAI_API_KEY")) {
            config.openai_api_key = env;
        }

        // --- agent section ---
        if (root["agent"]) {
            auto a = root["agent"];
            if (a["model"] && !a["model"].IsNull()) config.model = a["model"].as<std::string>();
            if (a["provider"] && !a["provider"].IsNull()) config.provider = a["provider"].as<std::string>();
            if (a["ollama_url"] && !a["ollama_url"].IsNull()) config.ollama_url = a["ollama_url"].as<std::string>();
            if (a["openai_api_key"] && !a["openai_api_key"].IsNull()) config.openai_api_key = a["openai_api_key"].as<std::string>();
            if (a["gemini_api_key"] && !a["gemini_api_key"].IsNull()) config.gemini_api_key = a["gemini_api_key"].as<std::string>();
        }

        // --- steps section ---
        if (root["steps"]) {
            auto s = root["steps"];
            for (const auto& step_name : {"decide_tool", "lookup_sales_data", "analyzing_data", "create_visualization"}) {
                if (s[step_name]) {
                    auto sn = s[step_name];
                    auto& sc = config.steps[step_name];
                    sc.step_name = step_name;

                    if (sn["n"] && !sn["n"].IsNull()) sc.n = sn["n"].as<int>();
                    if (sn["bon_param"] && !sn["bon_param"].IsNull()) sc.bon_param = sn["bon_param"].as<std::string>();
                    if (sn["temp_min"] && !sn["temp_min"].IsNull()) sc.temp_min = sn["temp_min"].as<double>();
                    if (sn["temp_max"] && !sn["temp_max"].IsNull()) sc.temp_max = sn["temp_max"].as<double>();
                    if (sn["max_tokens"] && !sn["max_tokens"].IsNull()) sc.max_tokens = sn["max_tokens"].as<int>();
                    if (sn["top_p_min"] && !sn["top_p_min"].IsNull()) sc.top_p_min = sn["top_p_min"].as<double>();
                    if (sn["top_p_max"] && !sn["top_p_max"].IsNull()) sc.top_p_max = sn["top_p_max"].as<double>();
                    if (sn["top_k_min"] && !sn["top_k_min"].IsNull()) sc.top_k_min = sn["top_k_min"].as<int>();
                    if (sn["top_k_max"] && !sn["top_k_max"].IsNull()) sc.top_k_max = sn["top_k_max"].as<int>();
                    if (sn["cot_n"] && !sn["cot_n"].IsNull()) sc.cot_n = sn["cot_n"].as<int>();
                    if (sn["eval"] && !sn["eval"].IsNull()) sc.eval_mode = sn["eval"].as<std::string>();
                    if (sn["use_cache"] && !sn["use_cache"].IsNull()) sc.use_cache = sn["use_cache"].as<bool>();
                    if (sn["cache_mode"] && !sn["cache_mode"].IsNull()) sc.cache_mode = sn["cache_mode"].as<std::string>();
                    if (sn["enabled"] && !sn["enabled"].IsNull()) sc.enabled = sn["enabled"].as<bool>();

                    if (sn["provider"] && !sn["provider"].IsNull()) sc.provider = sn["provider"].as<std::string>();
                    if (sn["model"] && !sn["model"].IsNull()) sc.model = sn["model"].as<std::string>();
                    if (sn["ollama_url"] && !sn["ollama_url"].IsNull()) sc.ollama_url = sn["ollama_url"].as<std::string>();
                }
            }
        }

        // --- schema section ---
        std::string data_dir = config.data_dir;
        if (root["schema"]) {
            auto sc = root["schema"];
            if (sc["data_dir"] && !sc["data_dir"].IsNull()) {
                data_dir = sc["data_dir"].as<std::string>();
            }
        }
        fs::path data_path(data_dir);
        if (!data_path.is_absolute()) {
            data_path = config_dir / data_path;
        }
        data_path = fs::weakly_canonical(data_path);
        config.data_dir = data_path.string();

        DatabaseSchema schema;
        try {
            schema = DatabaseSchema::from_data_dir(config.data_dir);
        } catch (const std::exception& e) {
            std::cerr << "[AgentConfig::from_yaml] Warning: could not load schema from "
                      << config.data_dir << ": " << e.what() << "\n";
        }

        // --- run section ---
        if (root["run"]) {
            auto r = root["run"];
            if (r["prompt"] && !r["prompt"].IsNull()) run_params.prompt = r["prompt"].as<std::string>();
            if (r["visualization_goal"] && !r["visualization_goal"].IsNull()) run_params.visualization_goal = r["visualization_goal"].as<std::string>();
            if (r["agent_mode"] && !r["agent_mode"].IsNull()) run_params.agent_mode = r["agent_mode"].as<std::string>();
            if (r["run_id"] && !r["run_id"].IsNull()) run_params.run_id = r["run_id"].as<std::string>();
            if (r["save_dir"] && !r["save_dir"].IsNull()) run_params.save_dir = r["save_dir"].as<std::string>();
            if (r["save_results"] && !r["save_results"].IsNull()) run_params.save_results = r["save_results"].as<bool>();
            if (r["save_execution_artifacts"] && !r["save_execution_artifacts"].IsNull()) run_params.save_execution_artifacts = r["save_execution_artifacts"].as<bool>();
            if (r["enable_codecarbon"] && !r["enable_codecarbon"].IsNull()) run_params.enable_codecarbon = r["enable_codecarbon"].as<bool>();
            if (r["reuse_from"] && !r["reuse_from"].IsNull()) run_params.reuse_from = r["reuse_from"].as<std::string>();
            if (r["interactive_config"] && !r["interactive_config"].IsNull()) run_params.interactive_config = r["interactive_config"].as<bool>();
        }

        // Mappatura agent_mode -> lookup_only e no_vis (logica esatta di Python)
        run_params.lookup_only = (run_params.agent_mode == "lookup_only");
        run_params.no_vis = (run_params.agent_mode == "lookup_only" || run_params.agent_mode == "analysis");
        config.mode = run_params.agent_mode;

        // --- tracing section ---
        if (root["tracing"]) {
            auto t = root["tracing"];
            if (t["enabled"] && !t["enabled"].IsNull()) run_params.tracing_enabled = t["enabled"].as<bool>();
            if (t["phoenix_endpoint"] && !t["phoenix_endpoint"].IsNull()) run_params.phoenix_endpoint = t["phoenix_endpoint"].as<std::string>();
            if (t["phoenix_api_key"] && !t["phoenix_api_key"].IsNull()) run_params.phoenix_api_key = t["phoenix_api_key"].as<std::string>();
            if (t["project_name"] && !t["project_name"].IsNull()) run_params.project_name = t["project_name"].as<std::string>();
        }

        return {config, run_params, schema};
    }
};

} // namespace agent_cpp
