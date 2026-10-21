# The rig's setup (PHONE, VM, DVMIPC, BUILD_HOST, BUILD_OUT; README.md) from rig.env next to the scripts, when there is one.
# Variables already in the environment win. rig.env stays out of git.
if [ -f "$(dirname "${BASH_SOURCE[0]}")/rig.env" ]; then
  while IFS='=' read -r key value; do
    case "$key" in ''|\#*) continue ;; esac
    [ -n "${!key+x}" ] || export "$key=$value"
  done < "$(dirname "${BASH_SOURCE[0]}")/rig.env"
fi
