#!/bin/bash
# oplus-ota-url.sh MODEL OTAVER -> final CDN URL of the latest full OTA (OPPO/OnePlus/realme CN).
# Needs `updater` from github.com/Houvven/OplusUpdater (UPDATER=path). OTAVER can be any
# older version string, e.g. PMB110_11.A.05_0050_202604200000. downloadCheck links are
# resolved with the stock OTA client headers (userId oplus-ota|16002018).
set -e
cd "${WORK:-.}"
${UPDATER:-updater} "$2" --model "$1" --carrier 10010111 --region CN 2>&1 | sed 's/\x1b\[[0-9;]*m//g' > "q-$1.json"
U=$(python3 -I -c "
import json,sys
j=json.load(open(sys.argv[1]))
def walk(o):
    if isinstance(o,dict):
        if 'manualUrl' in o: print(o['manualUrl']); raise SystemExit
        for v in o.values(): walk(v)
    elif isinstance(o,list):
        for x in o: walk(x)
walk(j)" "q-$1.json")
case "$U" in *downloadCheck*)
  U=$(curl -sS -I --max-time 30 -H 'userId: oplus-ota|16002018' -H 'User-Agent: okhttp/3.12.12' -H 'Accept: */*' "$U" | awk 'tolower($1)=="location:"{print $2}' | tr -d '\r');;
esac
echo "$U"
