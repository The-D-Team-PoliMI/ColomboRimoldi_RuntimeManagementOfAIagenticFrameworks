#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <agents-cpp/context.h>
#include <duckdb.h>
#include "cache.h"
#include "config.h"
#include "schema.h"
#include "steps.h"

namespace agent_cpp {

using JsonObject = nlohmann::json;
using StepResults = std::map<std::string, std::vector<JsonObject>>;

/**
 * @brief Opzioni di caching di una singola esecuzione dell'agente.
 */
struct RunOptions {
    std::string run_id;       // Identificativo della run
    std::string reuse_from;   // run_id da cui caricare i risultati degli step
    bool save_results = false; // Se true salva la run in cache
};

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
        SalesDataAgent(std::shared_ptr<agents::Context> context, const AgentConfig& config, const DatabaseSchema& schema, duckdb_connection con);

        /**
         * @brief Esegue il workflow dell'agente a partire da una richiesta in linguaggio naturale.
         * @param prompt Domanda o istruzione dell'utente.
         * @param opts Opzioni di caching (default: cache non usata né scritta).
         * @return JsonObject Stato finale contenente query SQL, dati CSV, risposte testuali ed eventuali errori.
         *         Contiene sempre `run_id`.
         */
        JsonObject run(const std::string& prompt, const RunOptions& opts = RunOptions());

        /**
         * @brief Esegue il workflow specificando un obiettivo di visualizzazione esplicito.
         * @param prompt Domanda principale dell'utente.
         * @param visualization_goal Indicazione specifica sul tipo di grafico desiderato.
         * @param opts Opzioni di caching (default: cache non usata né scritta).
         * @return JsonObject Stato finale dell'esecuzione.
         */
        JsonObject run_with_goal(const std::string& prompt, const std::string& visualization_goal, const RunOptions& opts = RunOptions());

    private:
        std::shared_ptr<agents::Context> context_;
        AgentConfig config_;
        DatabaseSchema schema_;
        duckdb_connection con_;
        RunCache cache_;
        StepResults cached_step_results_; // Risultati caricati dalla cache per questa run
        StepResults current_run_step_results_; // Risultati prodotti in questa run, che verranno salvati in cache

        /**
         * @brief Carica da cache i risultati da riusare in questa run.
         *
         * Con `reuse_from` carica quella run; altrimenti, solo se `save_results` è true,
         * usa la run col prompt più simile (se supera la soglia).
         */
        StepResults load_cached_results(const std::string& prompt, const RunOptions& opts);

        /**
         * @brief Cerca lo step nei risultati in cache e, se presente, lo restituisce al posto dell'esecuzione.
         * @return Lo stato da propagare, oppure std::nullopt in caso di cache miss (lo step va eseguito).
         */
        std::optional<JsonObject> lookup_cached_step(const std::string& step_name, const JsonObject& state, const StepConfig& sc);

        /**
         * @brief Scarta la cache degli step a valle di uno step appena rigenerato.
         *
         * Gli step sono in catena (lookup -> analisi -> visualizzazione): se i dati a monte cambiano,
         * i risultati a valle riusati dalla cache sarebbero incoerenti.
         */
        void invalidate_downstream_cache(const std::string& step_name);

        /**
         * @brief Salva la run corrente in cache. Un errore di salvataggio è solo un warning.
         */
        void save_run_to_cache(const std::string& run_id, const std::string& prompt, const std::string& visualization_goal, const JsonObject& final_state);

        /**
         * @brief Invoca direttamente la funzione corrispondente al nome dello step.
         * @param step_name Nome dello step da eseguire.
         * @param state Stato corrente della pipeline.
         * @return JsonObject Stato aggiornato dopo la singola esecuzione dello step.
         */
        JsonObject invoke_step_raw(const std::string& step_name, const JsonObject& state);

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
        JsonObject finalize_cot_diagnostics(JsonObject result, const JsonObject& base_state, const std::string& step_name, const JsonObject& diagnostics) const;

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
        JsonObject execute_step_with_config(const std::string& step_name, const JsonObject& state);
        /**
         * @brief Imposta sul client LLM i parametri di campionamento definiti nello StepConfig.
         * @param sc Configurazione dello step corrente.
         */
        void apply_step_options(const StepConfig& sc);
};

} 