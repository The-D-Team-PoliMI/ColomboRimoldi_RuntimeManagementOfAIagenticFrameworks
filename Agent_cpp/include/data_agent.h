#pragma once

#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>
#include <duckdb.h>
#include <agents-cpp/context.h>
#include "config.h"
#include "schema.h"

namespace agent_cpp {

using JsonObject = nlohmann::json;

/**
 * @brief Orchestratore principale dell'agente per l'interrogazione SQL, l'analisi e la visualizzazione dati.
 * Gestisce l'esecuzione del grafo, il Best-of-N sampling, CoT refinement e gli evaluators.
 */
class SalesDataAgent {
public:
    /**
     * @brief Costruisce una nuova istanza di SalesDataAgent.
     * @param context Contesto di esecuzione contenente il client LLM.
     * @param config Configurazione globale e per-step dell'agente.
     * @param schema Schema del database multi-tabella.
     * @param con Connessione attiva al database DuckDB in-memory.
     */
    SalesDataAgent(
        std::shared_ptr<agents::Context> context,
        const AgentConfig& config,
        const DatabaseSchema& schema,
        duckdb_connection con
    );

    /**
     * @brief Esegue il workflow dell'agente a partire da una richiesta in linguaggio naturale.
     */
    JsonObject run(const std::string& prompt);

    /**
     * @brief Esegue il workflow specificando un obiettivo di visualizzazione esplicito.
     */
    JsonObject run_with_goal(const std::string& prompt, const std::string& visualization_goal);

    /**
     * @brief Esegue il workflow completo, o in modalità lookup_only o no_vis (matching Python run_core).
     */
    JsonObject run(
        const std::string& prompt,
        const std::string& visualization_goal,
        bool lookup_only,
        bool no_vis
    );

    const AgentConfig& config() const { return config_; }
    AgentConfig& mutable_config() { return config_; }

private:
    std::shared_ptr<agents::Context> context_;
    AgentConfig config_;
    DatabaseSchema schema_;
    duckdb_connection con_;

    /**
     * @brief Invoca direttamente la funzione corrispondente al nome dello step.
     */
    JsonObject invoke_step_raw(const std::string& step_name, const JsonObject& state);

    /**
     * @brief Esegue uno step applicando Best-of-N, CoT refinement e valutazione (LLM judge / batch consensus).
     * Fedele al metodo _execute_step_with_config in Python (Agent/data_agent.py).
     */
    JsonObject execute_step_with_config(const std::string& step_name, const JsonObject& state);

    /**
     * @brief Applica le iterazioni CoT per un singolo candidato.
     */
    JsonObject apply_cot_iterations(
        const std::string& step_name,
        const JsonObject& base_state,
        const JsonObject& initial_result,
        int cot_n
    );

    /**
     * @brief Finalizza e ripulisce lo stato al termine delle iterazioni CoT di uno step.
     */
    JsonObject finalize_cot_diagnostics(
        JsonObject result,
        const JsonObject& base_state,
        const std::string& step_name,
        const JsonObject& diagnostics
    ) const;

    /**
     * @brief Imposta sul client LLM i parametri di campionamento specifici per il candidato.
     */
    void apply_candidate_options(
        const StepConfig& sc,
        double temp,
        double top_p,
        std::optional<int> top_k
    );
};

} // namespace agent_cpp
