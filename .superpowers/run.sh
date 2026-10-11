#!/usr/bin/env bash
# helper: run.sh start N | done N BASE -- cmd...
SK="/c/Users/samar/AppData/Roaming/Claude/local-agent-mode-sessions/3e165c44-69cf-466f-b41f-a9192a7c57e2/9d54b5e3-95f1-476f-9e7c-a278d37d72e1/rpm/plugin_01JETPTMWTgidsotmEhVkQTG/skills"
PLAN=docs/plans/2026-10-11-binance-lob-capture.md
cmd=$1; shift
case $cmd in
  start) bash "$SK/executing-plans/scripts/task-start" $PLAN "$1" && cat ".superpowers/sdd/2026-10-11-binance-lob-capture/task-$1-brief.md";;
  done) n=$1; base=$2; shift 3; bash "$SK/executing-plans/scripts/task-done" $PLAN $n $base -- "$@" 2>&1 | tail -2;;
esac
