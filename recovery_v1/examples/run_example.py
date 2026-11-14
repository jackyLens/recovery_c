"""读取一晚 JSON 输入并输出夜间指标：python3 examples/run_example.py [path.json]"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from recovery import NightRaw, process_night  # noqa: E402

path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "night_input_example.json")
with open(path, encoding="utf-8") as f:
    nm, segs = process_night(NightRaw.from_dict(json.load(f)))
print(json.dumps({k: (round(v, 3) if isinstance(v, float) else v) for k, v in nm.__dict__.items()},
                 ensure_ascii=False, indent=1, default=str))
