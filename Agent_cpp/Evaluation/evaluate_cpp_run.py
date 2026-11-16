#!/usr/bin/env python3
"""
Valuta con le funzioni GT di Agent/utils.py (Python) una run salvata dalla cache del C++.

Nessun porting: legge i JSON in <cache-dir>/<run_id>/ prodotti dall'agente C++ e chiama
make_csv_evaluator_gt / make_text_evaluator_gt / make_vis_evaluator_gt cosi' come sono.
La GT arriva da un benchmark_dataset*.json del progetto Python (campi gt_data, gt_analysis,
gt_chart_config, gt_chart_code, explicit_requirements).

Esempio:
    python evaluate_cpp_run.py --run-id 3ba08b42 \
        --cache-dir ../Agent_cpp/cache/agent_runs \
        --benchmark evaluation/benchmark_dataset.json \
        --utils Agent/utils.py \
        --judge-provider ollama --judge-model gemma3 --output scores.json
"""
import argparse
import importlib.util
import io
import json
from pathlib import Path
from typing import Any, Dict, List, Optional

import pandas as pd


def load_utils(utils_path: Path):
    """Carica Agent/utils.py direttamente dal file.

    `import Agent.utils` eseguirebbe Agent/__init__.py, che importa tutto l'agente
    (langchain, langgraph, ...). utils.py da solo richiede solo pandas e numpy; i client
    dei giudici LLM (langchain_openai / langchain_ollama) vengono importati solo al momento
    della chiamata.
    """
    spec = importlib.util.spec_from_file_location("agent_utils", utils_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_candidates(run_dir: Path, step_name: str) -> List[Dict[str, Any]]:
    """Legge la lista degli N candidati di uno step (N = 1 senza BoN). Lista vuota se manca."""
    path = run_dir / f"{step_name}.json"
    if not path.exists():
        return []
    data = json.loads(path.read_text(encoding="utf-8"))
    return data if isinstance(data, list) else []


def _norm_prompt(prompt: str) -> str:
    return " ".join(prompt.lower().split()).rstrip(".?! ")


def find_entry(dataset: List[Dict], prompt: str, index: Optional[int]) -> Dict:
    if index is not None:
        return dataset[index]
    matches = [e for e in dataset if _norm_prompt(e.get("prompt", "")) == _norm_prompt(prompt)]
    if not matches:
        raise SystemExit(
            f"Nessuna entry del benchmark ha questo prompt: {prompt!r}. "
            "Usa --index per scegliere l'entry a mano."
        )
    return matches[0]


def _score(fn, result: Dict, state: Dict) -> Dict[str, Any]:
    score = fn(result, state)
    reasoning = getattr(fn, "_store", {}).get("reasoning")
    return {"score": round(float(score), 4), "reasoning": reasoning}


def evaluate(run_dir: Path, entry: Dict, utils, *, text_metric: str, judge_provider: str,
             judge_model: str, ollama_url: str, skip: List[str]) -> Dict[str, List[Dict]]:
    lookup = load_candidates(run_dir, "lookup_sales_data")
    analysis = load_candidates(run_dir, "analyzing_data")
    viz = load_candidates(run_dir, "create_visualization")
    report: Dict[str, List[Dict]] = {}

    # --- lookup_sales_data: confronto del CSV con la GT (nessun LLM) -----------------------
    if lookup and entry.get("gt_data") and "csv" not in skip:
        gt_columns = [c.lower() for c in pd.read_csv(io.StringIO(entry["gt_data"])).columns]
        csv_eval = utils.make_csv_evaluator_gt(ground_truth_csv_text=entry["gt_data"])
        out = []
        for cand in lookup:
            result = dict(cand)
            # standardize_candidate_columns lavora su `data_df` (in Python lo costruisce lo step),
            # quindi lo ricostruisco dal CSV salvato dal C++.
            try:
                result["data_df"] = pd.read_csv(io.StringIO(result.get("data", "")))
            except Exception:
                result["data_df"], result["data"] = None, ""
            # Come in Python (data_agent.py:857): con gt_columns i nomi di colonna vengono
            # allineati alla GT e i valori normalizzati (numeri arrotondati) prima della
            # valutazione, senza LLM con un solo candidato. Serve a rendere i punteggi
            # confrontabili con quelli del benchmark Python: sulla tua run di prova, senza
            # questo passaggio il punteggio CSV e' 0.99 invece di 1.0 per i decimali.
            result = utils.standardize_candidate_columns([result], None, None, gt_columns=gt_columns)[0]
            out.append(_score(csv_eval, result, cand))
        report["lookup_sales_data"] = out

    # --- analyzing_data: testo contro la GT (giudice LLM, oppure bleu/spice) ---------------
    if analysis and entry.get("gt_analysis") and "text" not in skip:
        text_eval = utils.make_text_evaluator_gt(
            ground_truth_text=entry["gt_analysis"], metric=text_metric,
            judge_model=judge_model, provider=judge_provider, ollama_url=ollama_url,
        )
        state_before = lookup[0] if lookup else {}      # stato prima dello step (ne legge `error`)
        report["analyzing_data"] = [_score(text_eval, cand, state_before) for cand in analysis]

    # --- create_visualization: config e codice contro la GT (giudice LLM) ------------------
    if viz and entry.get("gt_chart_config") and "vis" not in skip:
        vis_eval = utils.make_vis_evaluator_gt(
            ground_truth_config=entry["gt_chart_config"],
            ground_truth_code=entry.get("gt_chart_code", ""),
            explicit_requirements=entry.get("explicit_requirements"),
            judge_model=judge_model, provider=judge_provider, ollama_url=ollama_url,
        )
        state_before = analysis[0] if analysis else (lookup[0] if lookup else {})
        report["create_visualization"] = [_score(vis_eval, cand, state_before) for cand in viz]

    return report


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run-id", required=True, help="run_id salvato dal C++ in cache")
    ap.add_argument("--cache-dir", default="cache/agent_runs", help="cartella della cache del C++")
    ap.add_argument("--benchmark", required=True, help="benchmark_dataset*.json con le GT")
    ap.add_argument("--utils", default="Agent/utils.py", help="percorso di Agent/utils.py del progetto Python")
    ap.add_argument("--index", type=int, help="entry del benchmark (default: cerca per prompt)")
    ap.add_argument("--text-metric", default="judge_gt", choices=["judge_gt", "bleu", "spice"])
    ap.add_argument("--judge-provider", default="openai", choices=["openai", "ollama"])
    ap.add_argument("--judge-model", default="gpt-4o-mini")
    ap.add_argument("--ollama-url", default="http://localhost:11434")
    ap.add_argument("--skip", nargs="*", default=[], choices=["csv", "text", "vis"],
                    help="valutazioni da saltare (es. --skip text vis per non usare il giudice LLM)")
    ap.add_argument("--output", help="file JSON in cui salvare il report")
    args = ap.parse_args()

    run_dir = Path(args.cache_dir) / args.run_id
    meta_path = run_dir / "metadata.json"
    if not meta_path.exists():
        raise SystemExit(f"Run non trovata: {run_dir}")
    prompt = json.loads(meta_path.read_text(encoding="utf-8")).get("prompt", "")

    dataset = json.loads(Path(args.benchmark).read_text(encoding="utf-8"))
    entry = find_entry(dataset, prompt, args.index)
    utils = load_utils(Path(args.utils))

    report = evaluate(run_dir, entry, utils, text_metric=args.text_metric,
                      judge_provider=args.judge_provider, judge_model=args.judge_model,
                      ollama_url=args.ollama_url, skip=args.skip)

    print(f"\nRun {args.run_id} | prompt: {prompt}")
    for step, items in report.items():
        for i, item in enumerate(items):
            tag = f"{step}[{i}]" if len(items) > 1 else step
            print(f"  {tag:<28} score = {item['score']:.4f}")
            if item["reasoning"]:
                print(f"      {item['reasoning']}")

    if args.output:
        Path(args.output).write_text(
            json.dumps({"run_id": args.run_id, "prompt": prompt, "scores": report}, indent=2, ensure_ascii=False),
            encoding="utf-8")
        print(f"\nReport salvato in {args.output}")


if __name__ == "__main__":
    main()