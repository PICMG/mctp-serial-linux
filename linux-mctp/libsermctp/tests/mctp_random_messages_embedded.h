#pragma once
#include <string>

// Returns the embedded JSON fixture used by tests when the external file
// isn't available on disk. The string is the full contents of
// `mctp_random_messages.json` and is intended only for test use.
std::string get_embedded_mctp_random_messages_json();
