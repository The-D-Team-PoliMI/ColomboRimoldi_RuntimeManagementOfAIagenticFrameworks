#pragma once
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace agent_cpp {

using JsonObject = nlohmann::json;
using StepResults = std::map<std::string, std::vector<JsonObject>>;

/**
 * @brief Cache persistente su disco dei risultati intermedi delle run dell'agente.
 *
 * Struttura su disco:
 * @code
 * cache/agent_runs/
 * ├── index.json                    # elenco run: run_id, prompt, timestamp
 * └── <run_id>/
 *     ├── metadata.json
 *     ├── decide_tool.json          # array di N stati per step (N = 1 senza BoN)
 *     ├── lookup_sales_data.json
 *     ├── analyzing_data.json
 *     └── create_visualization.json
 * @endcode
 */
class RunCache {
    public:

        /**
         * @brief Imposta la directory della cache.
         * @param cache_dir Directory in cui vengono salvate le run.
         */
        explicit RunCache(const std::string& cache_dir = "./cache/agent_runs");

        /**
         * @brief Salva una run completa: un file JSON per step, `metadata.json` e aggiornamento dell'indice.
         * @param run_id Identificativo della run (nome della cartella: niente '/' o '\\').
         * @param prompt Prompt dell'utente.
         * @param agent_config Configurazione dell'agente (non deve contenere API key).
         * @param step_results Mappa nome_step -> lista degli N risultati.
         * @param final_result Stato finale restituito dall'agente.
         * @param metadata Metadati aggiuntivi.
         * @throws std::invalid_argument se il run_id non è valido.
         * @throws std::runtime_error se la scrittura su disco fallisce.
         */
        void save_run(const std::string& run_id, const std::string& prompt, const JsonObject& agent_config,
                      const StepResults& step_results, const JsonObject& final_result, const JsonObject& metadata = JsonObject::object());

        /**
         * @brief Trova le run con prompt simile (similarità di Jaccard sull'insieme di parole).
         * @param prompt Prompt di riferimento.
         * @param top_k Numero massimo di run restituite.
         * @param similarity_threshold Similarità minima in [0, 1].
         * @return run_id ordinati per similarità decrescente.
         */
        std::vector<std::string> find_similar_runs(const std::string& prompt, std::size_t top_k = 5, double similarity_threshold = 0.6) const;

        /**
         * @brief Carica i risultati di tutti gli step di una run 
         * @return Mappa nome_step -> risultati; vuota se la run non esiste.
         */
        StepResults load_all_step_results(const std::string& run_id) const;

    private:
        std::filesystem::path cache_dir_;
        std::filesystem::path index_path_;

        // Carica da disco la lista dei risultati di uno step (nullopt se manca, è illeggibile o vuota).
        std::optional<std::vector<JsonObject>> load_step_results(const std::string& run_id, const std::string& step_name) const;
        // Legge index.json; se manca o è corrotto restituisce {"runs": []}
        JsonObject load_index() const;
        // Scrive index.json
        void save_index(const JsonObject& index) const;
};

}
