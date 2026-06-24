#include "parser.hpp"

// MessageParser now default-constructs (its simdjson parser_ owns its own buffers,
// reused across parses — the single most important simdjson optimization). Frames
// are parsed in place via parse_padded on the SPSC ring slot, so there is no
// owned scratch buffer to prefault here. This TU is intentionally minimal.
