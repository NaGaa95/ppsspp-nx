# libnx's in6_addr union puts 16-bit words first. ENet's positional byte
# initializers need to name s6_addr explicitly. Keep the pinned submodule intact.
set(SWITCH_POSTOFFICE_DIR "${CMAKE_CURRENT_BINARY_DIR}/switch-postoffice")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/aemu_postoffice/"
	DESTINATION "${SWITCH_POSTOFFICE_DIR}" PATTERN ".git" EXCLUDE)
set(_switch_enet "${SWITCH_POSTOFFICE_DIR}/ext/enet/enet.h")
file(READ "${_switch_enet}" _switch_enet_source)
string(REGEX REPLACE
	"(static const struct in6_addr [^=]+=[ ]*\\{\\{)\\{"
	"\\1.s6_addr = {" _switch_enet_source "${_switch_enet_source}")
file(WRITE "${_switch_enet}" "${_switch_enet_source}")
