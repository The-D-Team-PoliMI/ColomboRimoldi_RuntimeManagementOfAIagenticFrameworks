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

int main() {
    AgentConfig config;

    // Recupero dell'API key corretta in base al provider
    std::string api_key = "";
    if (config.provider == "google") {
        if (!config.gemini_api_key.empty())
            api_key = config.gemini_api_key;
        else
            std::cerr << "Attenzione: GEMINI_API_KEY non trovata in config o variabile d'ambiente.\n";
    }

    auto llm = agents::createLLM(config.provider, api_key, config.model);
    if (config.provider == "ollama") {
        llm->setApiBase(config.ollama_url);
    }

    auto options = llm->getOptions();
    options.timeout_ms = 600000;
    llm->setOptions(options);

    auto context = std::make_shared<agents::Context>();
    context->setLLM(llm);

    // Caricamento schema database
    DatabaseSchema schema;
    try {
        schema = DatabaseSchema::from_data_dir(config.data_dir);
    } catch (const std::exception& e) {
        std::cerr << "Errore caricamento schema: " << e.what() << std::endl;
        return 1;
    }

    // Inizializzazione DuckDB
    duckdb_database db = nullptr;
    duckdb_connection con = nullptr;
    if (duckdb_open(nullptr, &db) == DuckDBError || duckdb_connect(db, &con) == DuckDBError || !init_database(con, schema)) {
        std::cerr << "Errore inizializzazione DuckDB." << std::endl;
        return 1;
    }

    // Avvio del workflow
    SalesDataAgent agent(context, config, schema, con);
    JsonObject result = agent.run_with_goal(
        "Show promo vs non-promo revenue in 2023 as a bar chart.",
        "bar chart comparing total revenue for promo and non-promo sales in 2023."
    );

    if (result.contains("error") && !result["error"].get<std::string>().empty()) {
        std::cout << "\n[Pipeline Error]: " << result["error"].get<std::string>() << "\n";
    } else {
        std::cout << "Risultati generati in answer: " << result["answer"].size() << "\n";
    }

    duckdb_disconnect(&con);
    duckdb_close(&db);
    return 0;
}