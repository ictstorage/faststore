#include "cluster/cluster.hpp"
#include "misc/misc.hpp"

#include <iostream>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace Hill::Cluster;
using namespace Hill::Misc;
auto test_serialization() -> void {
    ClusterMeta meta;
    meta.cluster.node_num = 2;
    meta.version = 4321;
    for (size_t i = 0; i < meta.cluster.node_num; i++) {
        meta.cluster.nodes[i].version = 1234;
        meta.cluster.nodes[i].node_id = i + 1;
        meta.cluster.nodes[i].total_pm = 0x12345678;
        meta.cluster.nodes[i].available_pm = 0x1234;
        meta.cluster.nodes[i].is_active = true;
        meta.cluster.nodes[i].addr.content[0] = 127;
        meta.cluster.nodes[i].addr.content[1] = 0;
        meta.cluster.nodes[i].addr.content[2] = 0;
        meta.cluster.nodes[i].addr.content[3] = i + 1;
    }

    meta.group.add_main("start", 1);
    meta.group.add_main("start start", 2);
    meta.dump();

    ClusterMeta meta2;
    meta2.deserialize(meta.serialize().get());
    meta2.dump();
}

auto test_network_serialization() -> void {
    std::thread server([&]() {
        ClusterMeta meta;
        meta.cluster.node_num = 2;
        meta.version = 4321;
        for (size_t i = 0; i < meta.cluster.node_num; i++) {
            meta.cluster.nodes[i].version = 1234;
            meta.cluster.nodes[i].node_id = i + 1;
            meta.cluster.nodes[i].total_pm = 0x12345678;
            meta.cluster.nodes[i].available_pm = 0x1234;
            meta.cluster.nodes[i].is_active = true;
            meta.cluster.nodes[i].addr.content[0] = 127;
            meta.cluster.nodes[i].addr.content[1] = 0;
            meta.cluster.nodes[i].addr.content[2] = 0;
            meta.cluster.nodes[i].addr.content[3] = i + 1;
        
        
        }
        meta.group.add_main("start", 1);
        meta.group.add_main("start start", 2);
        meta.dump();
        
        auto sock = socket_connect(true, 2333, nullptr);
        auto total = meta.total_size();
        write(sock, &total, sizeof(total));
        write(sock, meta.serialize().get(), total);
        shutdown(sock, 0);
    });

    std::thread client([&]() {
        ClusterMeta meta2;
        sleep(1);
        auto sock = socket_connect(false, 2333, "127.0.0.1");
        auto total = 0UL;
        read(sock, &total, sizeof(total));
        auto buf = new uint8_t[total];
        read(sock, buf, total);
        meta2.deserialize(buf);
        meta2.dump();
        shutdown(sock, 0);
    });

    server.join();
    client.join();
}


auto main() -> int {
    std::cout << "local serialization\n";
    test_serialization();
    std::cout << "\n>> network serialization\n";
    test_network_serialization();

    Node n1;
    n1.prepare("./node1.info");
    n1.dump();
}
