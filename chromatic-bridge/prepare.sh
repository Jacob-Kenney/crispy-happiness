#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
source="$root/.chromatic/shiny-relay-src-1.1.12/mcu"
target="$root/.chromatic/bridge-build/mcu"
mkdir -p "$target"
cp -R "$source/." "$target/"
mkdir -p "$target/components/bridge"
cp "$root"/chromatic-bridge/*.c "$root"/chromatic-bridge/*.h \
    "$root"/chromatic-bridge/CMakeLists.txt "$root"/chromatic-bridge/idf_component.yml "$target/components/bridge/"
cd "$target"
sed -i.bak '/#include "cwl.h"/a\
#include "udp_ws_bridge.h"
' main/main.c
sed -i.bak '/esp_console_register_help_command();/a\
    bridge_register();
' main/main.c
sed -i.bak '/ESP_ERROR_CHECK(esp_console_start_repl(pRepl));/i\
    bridge_restore();
' main/main.c
sed -i.bak 's/console crc cwl nw sdcard/console crc cwl nw sdcard bridge/' main/CMakeLists.txt
sed -i.bak '1i\
#include "udp_ws_bridge.h"
' main/pwrmgr.c
sed -i.bak 's/if (CWL_KeepAwake() || CfDev_KeepAwake()/if (bridge_keep_awake() || CWL_KeepAwake() || CfDev_KeepAwake()/' main/pwrmgr.c
sed -i.bak '/void CWL_SetNetworkWanted(bool On);/a\
void CWL_SetBridgeActive(bool active);
' components/cwl/cwl.h
sed -i.bak '/static volatile bool _NwWanted/a\
static volatile bool _BridgeActive = false;
' components/cwl/cwl_esp.c
sed -i.bak '/^static bool WantAssoc(void)/i\
static bool BridgeWifiWanted(void) { return strcmp(_Settings.Ssid, "GameBoy") == 0; }
' components/cwl/cwl_esp.c
sed -i.bak 's/_RecvHoldF || TimeWantsRadio()/_RecvHoldF || _BridgeActive || TimeWantsRadio()/g
s/_NwWanted || CwlOnline_NeedsRadio()/_NwWanted || _BridgeActive || CwlOnline_NeedsRadio()/g
s/(_NwWanted || _RecvHoldF/(_NwWanted || _BridgeActive || _RecvHoldF/g
s/_BridgeActive || /_BridgeActive || BridgeWifiWanted() || /g
s/if (_RadioStarted \&\& _Associate) CwlNet_AutoJoinPoll/if (_RadioStarted \&\& _Associate \&\& !_BridgeActive \&\& !BridgeWifiWanted()) CwlNet_AutoJoinPoll/' components/cwl/cwl_esp.c
cat >> components/cwl/cwl_esp.c <<'END'

void CWL_SetBridgeActive(bool active)
{
    _BridgeActive = active;
    if (_Task) xTaskNotifyGive(_Task);
}
END
cat > sdkconfig.bridge <<'END'
CONFIG_APP_PROJECT_VER="1.1.12-bridge-persist"
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_LWIP_NETIF_LOOPBACK=y
CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y
END
