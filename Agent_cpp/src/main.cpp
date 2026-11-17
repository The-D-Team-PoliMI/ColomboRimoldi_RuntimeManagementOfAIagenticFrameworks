#include <iostream>
#include <memory>
#include <cstdlib>
#include <agents-cpp/context.h>
#include <agents-cpp/llm_interface.h>
#include "config.h"
#include "schema.h"
#include "utils.h"
#include "data_agent.h"

using namespace agent_cpp;

int main(int argc, char** argv) {
    std::string config_path = (argc > 1) ? argv[1] : "config/run_config.yaml";
    std::cout << "Loading configuration from: " << config_path << std::endl;

    AgentConfig config;
    RunParams run_params;
    DatabaseSchema schema;

    try {
        std::tie(config, run_params, schema) = AgentConfig::from_yaml(config_path);
    } catch (const std::exception& e) {
        std::cerr << "Errore caricamento configurazione YAML: " << e.what() << std::endl;
        return 1;
    }

    std::string api_key = "";
    if (config.provider == "google") {
        if (!config.gemini_api_key.empty()) {
            api_key = config.gemini_api_key;
        } else if (const char* env_key = std::getenv("GEMINI_API_KEY")) {
            api_key = env_key;
        } else {
            std::cerr << "Attenzione: GEMINI_API_KEY non trovata in config o variabile d'ambiente.\n";
        }
    } else if (config.provider == "openai") {
        if (!config.openai_api_key.empty()) {
            api_key = config.openai_api_key;
        } else if (const char* env_key = std::getenv("OPENAI_API_KEY")) {
            api_key = env_key;
        } else {
            std::cerr << "Attenzione: OPENAI_API_KEY non trovata in config o variabile d'ambiente.\n";
        }
    }

    std::cout << "Provider: " << config.provider << " | Model: " << config.model << std::endl;
    std::cout << "Prompt: " << run_params.prompt << std::endl;
    std::cout << "Mode: lookup_only=" << (run_params.lookup_only ? "true" : "false")
              << ", no_vis=" << (run_params.no_vis ? "true" : "false") << std::endl;
    if (run_params.run_id.has_value()) {
        std::cout << "Run ID: " << run_params.run_id.value() << std::endl;
    }
    std::cout << "------------------------------------------------------------" << std::endl;

    auto llm = agents::createLLM(config.provider, api_key, config.model);
    if (config.provider == "ollama") {
        llm->setApiBase(config.ollama_url);
    }

    auto options = llm->getOptions();
    options.timeout_ms = 600000;
    llm->setOptions(options);

    auto context = std::make_shared<agents::Context>();
    context->setLLM(llm);

    // Inizializzazione DuckDB
    duckdb_database db = nullptr;
    duckdb_connection con = nullptr;
    if (duckdb_open(nullptr, &db) == DuckDBError || duckdb_connect(db, &con) == DuckDBError || !init_database(con, schema)) {
        std::cerr << "Errore inizializzazione DuckDB." << std::endl;
        return 1;
    }

    // Avvio del workflow dell'agente
    SalesDataAgent agent(context, config, schema, con);

    std::string goal = run_params.visualization_goal.value_or("");
    JsonObject result = agent.run(
        run_params.prompt,
        goal,
        run_params.lookup_only,
        run_params.no_vis
    );

    std::cout << "\n============================================================" << std::endl;
    std::cout << "RESULTS" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (result.contains("error") && !result["error"].get<std::string>().empty()) {
        std::cout << "Error: " << result["error"].get<std::string>() << "\n";
    }
    if (result.contains("answer") && result["answer"].is_array()) {
        for (size_t i = 0; i < result["answer"].size(); ++i) {
            std::cout << "\n--- Step " << (i + 1) << " ---\n";
            std::cout << result["answer"][i].get<std::string>() << "\n";
        }
    }
    if (result.contains("sql_query") && !result["sql_query"].get<std::string>().empty()) {
        std::cout << "\nSQL: " << result["sql_query"].get<std::string>() << "\n";
    }
    if (result.contains("data") && !result["data"].get<std::string>().empty()) {
        std::string preview = result["data"].get<std::string>();
        if (preview.size() > 500) preview = preview.substr(0, 500) + "...";
        std::cout << "\nData preview:\n" << preview << "\n";
    }

    duckdb_disconnect(&con);
    duckdb_close(&db);
    return 0;
}
