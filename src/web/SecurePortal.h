#pragma once
#include "core/ConfigStore.h"

namespace SecurePortal {
void update(ConfigStore& store);
String certificate();
bool webhookAllows(const String& url, const String& secret, IPAddress ip);
}
