#pragma once
#include <string>
#include <unordered_map>

namespace agent_cpp {

/**
 * @brief Configurazione degli iperparametri di generazione e CoT per un singolo nodo del grafo.
 */
struct StepConfig {
    double temperature = 0.15;
    double top_p = 0.95;
    int max_tokens = 5000;
    int cot_n = 1;
    bool enabled = true;
};

/**
 * @brief Configurazione globale dell'agente.
 */
struct AgentConfig {
    std::string provider = "google";
    std::string model = "gemini-3.5-flash-lite"; 
    std::string ollama_url = "http://127.0.0.1:11434/api";
    std::string gemini_api_key = ""; // chaive api 
    std::string mode = "full"; // Modalità: "full", "analysis", "lookup_only"
    std::string data_dir = "../data";

    // Configurazioni specifiche per ogni nodo.
    std::unordered_map<std::string, StepConfig> steps = {
        {"decide_tool",          StepConfig{.temperature = 0.15, .max_tokens = 1000, .cot_n = 1}},
        {"lookup_sales_data",    StepConfig{.temperature = 0.15, .max_tokens = 5000, .cot_n = 1}}, 
        {"analyzing_data",       StepConfig{.temperature = 0.15, .max_tokens = 7000, .cot_n = 1}},
        {"create_visualization", StepConfig{.temperature = 0.15, .max_tokens = 7000, .cot_n = 1}}
    };

    /**
     * @brief Restituisce la configurazione associata a uno specifico step.
     * @param step_name Nome dello step (es. "lookup_sales_data").
     * @return StepConfig Configurazione dello step, oppure StepConfig di default se non trovato.
     */
    StepConfig get_step_config(const std::string& step_name) const{
        auto it = steps.find(step_name);
        if (it != steps.end())
            return it->second;
        return StepConfig{};
    }
};

}