make
sudo ip link delete tap100
sudo ip tuntap add tap100 mode tun
sudo ip addr add 10.0.0.1/24 dev tap100
sudo ip link set dev tap100 up
# sudo bash ./rtsh.sh tenders/spt/solo5-spt --net:service0=tap100 ./tests/test_net_pong/test_net_pong.spt verbose 1000
sudo bash ./rtsh.sh tenders/spt/solo5-spt --net:service0=tap100 ./tests/test_net_pong/test_net_pong.spt 


# Test from cmdline:
# nc -s 10.0.0.1 -u 10.0.0.2 8000 -vvv