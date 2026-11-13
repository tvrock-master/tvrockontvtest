// Keep optional event ABI declarations aligned with the updated SDK (CC0).
#include <windows.h>
#include <dshow.h>
#define TVTEST_PLUGIN_VERSION TVTEST_PLUGIN_VERSION_(0,0,14)
#include "../TVTestPlugin.h"
#include <cstddef>
static_assert(TVTest::EVENT_FILTERGRAPH_INITIALIZED==33,"graph initialization ABI");
static_assert(TVTest::EVENT_FILTERGRAPH_FINALIZE==34,"graph finalization ABI");
static_assert(offsetof(TVTest::FilterGraphInfo,pGraphBuilder)==8,"graph pointer ABI");
static_assert(sizeof(TVTest::FilterGraphInfo)==8+sizeof(void *),"graph info ABI");
static_assert(TVTest::CHANNELINFO_SIZE_V2==284,"old ChannelInfo ABI");
