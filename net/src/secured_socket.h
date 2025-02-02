#pragma once


#include <mutex>
#include <vector>
#include <string>
#include <ag_defs.h>
#include <ag_socket.h>
#include <ag_logger.h>
#include "tls_codec.h"


namespace ag {


class secured_socket : public socket {
public:
    secured_socket(socket_factory::socket_ptr underlying_socket,
            const certificate_verifier *cert_verifier,
            socket_factory::secure_socket_parameters secure_parameters);
    ~secured_socket() override = default;

private:
    enum state : int;

    state state;
    with_mtx<callbacks> callbacks;
    socket_factory::socket_ptr underlying_socket;
    tls_codec codec;
    std::string sni;
    std::vector<std::string> alpn;
    logger log;

    [[nodiscard]] std::optional<evutil_socket_t> get_fd() const override;
    [[nodiscard]] std::optional<error> connect(connect_parameters params) override;
    [[nodiscard]] std::optional<error> send(uint8_view data) override;
    [[nodiscard]] std::optional<error> send_dns_packet(uint8_view data) override;
    [[nodiscard]] bool set_timeout(std::chrono::microseconds timeout) override;
    [[nodiscard]] std::optional<error> set_callbacks(struct callbacks cbx) override;

    static void on_connected(void *arg);
    static void on_read(void *arg, uint8_view data);
    static void on_close(void *arg, std::optional<error> error);

    connect_parameters make_underlying_connect_parameters(connect_parameters &params) const;
    struct callbacks get_callbacks();
    std::optional<error> flush_pending_encrypted_data();
};


} // namespace ag
