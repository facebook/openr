#!/usr/bin/env bash
set -x

rtr_name=$1
route_batch_flag=$2

pid=0

# SIGUSR1-handler
my_handler() {
  echo "my_handler"
}

# SIGTERM-handler
term_handler() {
  if [ $pid -ne 0 ]; then
    kill -SIGTERM "$pid"
    wait "$pid"
  fi
  exit 143; # 128 + 15 -- SIGTERM
}

# setup handlers
# on callback, kill the last background process, which is `tail -f /dev/null` and execute the specified handler
trap 'kill ${!}; my_handler' SIGUSR1
trap 'kill ${!}; term_handler' SIGTERM

# run application
/root/${rtr_name}/run_openr.sh $route_batch_flag > /root/${rtr_name}/openr.log 2>&1 &
pid="$!"

# wait forever
while true
do
  tail -f /dev/null & wait ${!}
done
