make
sudo ip tuntap add tap100 mode tun
sudo ip addr add 10.0.0.1/24 dev tap100
sudo ip link set dev tap100 up
sudo bash ./rtsh.sh tenders/spt/solo5-spt --net:service0=tap100 ./tests/test_net_cyclic/test_net_cyclic.spt 1000