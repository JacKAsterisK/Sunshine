# Generate a reproducible header overlay instead of changing the upstream submodule.
# The socket accessor keeps the connection alive and binds TLS identity to each request.
set(_simple_web_source "${CMAKE_SOURCE_DIR}/third-party/Simple-Web-Server")
set(_simple_web_overlay "${CMAKE_BINARY_DIR}/sunshine-web-headers/Simple-Web-Server")
file(MAKE_DIRECTORY "${_simple_web_overlay}")
file(GLOB _simple_web_headers "${_simple_web_source}/*.hpp")
foreach(_header IN LISTS _simple_web_headers)
    get_filename_component(_name "${_header}" NAME)
    configure_file("${_header}" "${_simple_web_overlay}/${_name}" COPYONLY)
endforeach()
file(READ "${_simple_web_source}/server_http.hpp" _simple_web_http)
string(REPLACE "\r\n" "\n" _simple_web_http "${_simple_web_http}")
set(_simple_web_anchor "      asio::ip::tcp::endpoint remote_endpoint() const noexcept {")
string(FIND "${_simple_web_http}" "${_simple_web_anchor}" _simple_web_position)
string(REPLACE "${_simple_web_anchor}" "" _simple_web_without_anchor "${_simple_web_http}")
string(LENGTH "${_simple_web_http}" _simple_web_length)
string(LENGTH "${_simple_web_without_anchor}" _simple_web_without_length)
string(LENGTH "${_simple_web_anchor}" _simple_web_anchor_length)
math(EXPR _simple_web_count "(${_simple_web_length} - ${_simple_web_without_length}) / ${_simple_web_anchor_length}")
if(_simple_web_position LESS 0 OR NOT _simple_web_count EQUAL 1)
    message(FATAL_ERROR "Simple-Web-Server Request changed; review Sunshine's TLS socket accessor overlay")
endif()
set(_simple_web_accessor [=[      /**
       * @brief Borrow this request's socket while retaining its owning connection.
       * @return The live socket, or an empty pointer after connection destruction.
       * @note Inspect TLS state on the request's executor; do not perform concurrent socket I/O.
       */
      std::shared_ptr<socket_type> connection_socket() const noexcept {
        if(auto owner = connection.lock())
          return std::shared_ptr<socket_type>(owner, owner->socket.get());
        return {};
      }

]=])
string(REPLACE "${_simple_web_anchor}" "${_simple_web_accessor}${_simple_web_anchor}" _simple_web_http "${_simple_web_http}")
file(WRITE "${_simple_web_overlay}/server_http.hpp" "${_simple_web_http}")
# Sunshine uses <Simple-Web-Server/...>; copied quoted includes stay inside the overlay.
include_directories(BEFORE "${CMAKE_BINARY_DIR}/sunshine-web-headers")
target_include_directories(simple-web-server BEFORE INTERFACE "${CMAKE_BINARY_DIR}/sunshine-web-headers")
