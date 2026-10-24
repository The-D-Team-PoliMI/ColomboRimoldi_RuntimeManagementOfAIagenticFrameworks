#pragma once
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include <agents-cpp/context.h>
#include <duckdb.h>
#include "config.h"
#include "schema.h"
#include "steps.h"

namespace agent_cpp {

using JsonObject = nlohmann::json;

/**
 * @brief Orchestratore principale dell'agente per l'interrogazione SQL, l'analisi e la visualizzazione dati.
 * 
 * Implementa il grafo di stati gestendo le opzioni per-step e il ciclo di raffinamento iterativo CoT.
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
        SalesDataAgent(std::shared_ptr<agents::Context> context, const AgentConfig& config, 
                       const DatabaseSchema& schema, duckdb_connection con);

        /**
         * @brief Esegue il workflow dell'agente a partire da una richiesta in linguaggio naturale.
         * @param prompt Domanda o istruzione dell'utente.
         * @return JsonObject Stato finale contenente query SQL, dati CSV, risposte testuali ed eventuali errori.
         */
        JsonObject run(const std::string& prompt);

        /**
         * @brief Esegue il workflow specificando un obiettivo di visualizzazione esplicito.
         * @param prompt Domanda principale dell'utente.
         * @param visualization_goal Indicazione specifica sul tipo di grafico desiderato.
         * @return JsonObject Stato finale dell'esecuzione.
         */
        JsonObject run_with_goal(const std::string& prompt, const std::string& visualization_goal);

    private:
        std::shared_ptr<agents::Context> context_;
        AgentConfig config_;
        DatabaseSchema schema_;
        duckdb_connection con_;

        /**
         * @brief Invoca direttamente la funzione corrispondente al nome dello step.
         * @param step_name Nome dello step da eseguire.
         * @param state Stato corrente della pipeline.
         * @return JsonObject Stato aggiornato dopo la singola esecuzione dello step.
         */
        JsonObject invoke_step_raw(const std::string& step_name, JsonObject& state);

        /**
         * @brief Finalizza e ripulisce lo stato al termine delle iterazioni CoT di uno step.
         * 
         * Rimuove dallo stato finale le chiavi interne temporanee utilizzate durante i cicli
         * di raffinamento (_cot_previous_response, _cot_execution_error) e aggrega i dati
         * diagnostici dello step corrente nella mappa _cot_diagnostics_per_step.
         * 
         * @param result Stato JSON prodotto dall'ultima iterazione del nodo.
         * @param base_state Stato JSON iniziale prima dell'avvio delle iterazioni CoT.
         * @param step_name Nome dello step eseguito.
         * @param diagnostics Oggetto JSON contenente le metriche di telemetria e convergenza dello step.
         * @return JsonObject Stato consolidato e ripulito, pronto per essere propagato al nodo successivo.
         */
        JsonObject finalize_cot_diagnostics(JsonObject result, const JsonObject& base_state, 
                                        const std::string& step_name, const JsonObject& diagnostics) const;

         /**
         * @brief Applica le iterazioni dellaa CoT.
         * 
         * Re-invoca lo step iniettando l'output precedente (o l'errore di esecuzione) nel prompt
         * e interrompe anticipatamente il ciclo se la similarità supera COT_SIMILARITY_THRESHOLD.
         * 
         * @param step_name Nome dello step corrente.
         * @param base_state Stato pulito di partenza prima dello step.
         * @param initial_result Risultato prodotto dalla prima iterazione.
         * @param cot_n Numero totale massimo di iterazioni previste.
         * @return JsonObject Risultato finale.
         */
        JsonObject apply_cot_iterations(const std::string& step_name, const JsonObject& base_state, const JsonObject& initial_result, int cot_n);

        /**
         * @brief Configura i parametri LLM dello step, esegue la chiamata iniziale e avvia il CoT.
         * @param step_name Nome dello step da eseguire.
         * @param state Stato corrente dell'agente.
         * @return JsonObject Stato aggiornato al termine dello step.
         */
        JsonObject execute_step_with_config(const std::string& step_name, JsonObject& state);

        /**
         * @brief Imposta sul client LLM i parametri di campionamento definiti nello StepConfig.
         * @param sc Configurazione dello step corrente.
         */
        void apply_step_options(const StepConfig& sc);
};

} 