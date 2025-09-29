## p1ping usage
The program is given one or more arguments. All the arguments are simulated IP addresses. The first argument is the IP address for the machine being simulated by `p1ping` -- this machine only has one interface. The remaining arguments are simulated IP addresses which `p1ping` should send pings to. `p1ping` does so rather slowly (one packet every 5 seconds or so) to give you time to see what is happening, and also prints out the ICMP packets it sends or receives (and the results of its error checking on any other packet).

Because `p1ping` is based on simnet, it requires a simconfig file with a single line describing the router it is connected to. `p1ping` will only open this one interface. 

### Sample calls:
``` bash
./p1ping 1:2:3:4::1
# use IP address 1:2:3:4::1 and wait for others to ping us
```

```bash
./p1ping 1:2:3:4::1 1:2:3:4::2 9:8:7:6::3
# use IP address 1:2:3:4::1 and ping 1:2:3:4::2 then (after 5s) ping 9:8:7:6::3, then wait for others to ping us.
```

I have also added an optional switch that allows you to specify the hop limit with which packets are originated (the hop limit for Echo Reply packets is always 60). To use this option, specify `-h hop_limit` as the first two arguments, e.g. to specify a hop limit of 5 in the second example above, use:

```bash
       p1ping -h 5 1:2:3:4::1 1:2:3:4::2 9:8:7:6::3
```