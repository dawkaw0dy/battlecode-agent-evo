"""M0 probe: combine the job artifacts into one results file (run inside Actions)."""
import glob
import json
import os
import sys


def main(parts, target):
    slots = [json.load(open(f)) for f in sorted(glob.glob(os.path.join(parts, 'slot-*.json')))]
    events = sorted([(s['start'], 1) for s in slots] + [(s['end'], -1) for s in slots])
    running = peak = 0
    for _, step in events:
        running += step
        peak = max(peak, running)
    json.dump({'trigger_sha': os.environ['GITHUB_SHA'], 'run_id': os.environ['GITHUB_RUN_ID'],
               'run_attempt': os.environ['GITHUB_RUN_ATTEMPT'],
               'games': [json.load(open(f)) for f in sorted(glob.glob(os.path.join(parts, 'games-*.json')))],
               'concurrency': {'slots': slots, 'peak_parallel': peak}},
              open(target, 'w'), indent=1)


if __name__ == '__main__':
    main(*sys.argv[1:3])
