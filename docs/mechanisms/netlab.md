# netlab: how it works

netlab (design F14) gives a cluster test a real, lossy network on one Linux host, with the proxy
tier's interface.

```
n1: to2 10.1.2.1  <=== veth ===>  to1 10.1.2.2 :n2
n1: to3 10.1.3.1  <=== veth ===>  to1 10.1.3.3 :n3
n2: to3 10.2.3.2  <=== veth ===>  to2 10.2.3.3 :n3
each end: netem on its egress; each namespace: nft set "cut", `iifname @cut drop`
```

**Topology.** One namespace per node and one veth pair per pair of nodes, made directly inside the
two namespaces, so nothing ever sits in the root namespace. Node `x`'s end of the link to `y` is
`to<y>` at `10.<lo>.<hi>.<x>/24`; `address(source, target)` is `target`'s end of their link, so
every directed pair has its own path and its own faults. A node listens on its port in its own
namespace (`command(node, argv)` starts it there; `socket(node)` makes a socket there by entering
the namespace with `setns` and coming back).

**netem.** Each veth end's root qdisc is netem with a 100,000-packet queue, so a long delay at a
high rate adds no loss of its own. Every shaping call rewrites the qdiscs it touches from the
link's whole shape (delay, loss, reorder, duplicate, rate), one `tc -batch` per namespace. netem
reorders by sending a packet ahead of the delay, so `reorder` without a delay raises.

**nftables.** Each namespace has `table inet faultlab` with a set `cut` of interface names and
one input rule, `iifname @cut drop`. A partition of `(a, b)` adds `to<a>` to b's set and `to<b>`
to a's: one element per directed pair, dropped where it arrives, so the sender sees a silent
network and TCP retransmits after heal, as the proxy delivers what it held.

**reset.** `ss -K` destroys the pair's TCP sockets on both sides; the kernel sends each peer a
RST and the local socket reports the abort. Unlike a reject rule, it reaches idle connections at
once and leaves new connections working, as `Mesh.reset` does. netlab then lists the pair's
sockets and raises when any survived (a kernel without `CONFIG_INET_DIAG_DESTROY`).

**Teardown.** Deleting a namespace deletes its veths, qdiscs and table, so teardown is one
`ip -force -batch` of `netns delete`. It runs at `close`, at exit (atexit), and on SIGTERM and
SIGHUP when their handlers are the default (SIGINT exits through atexit). SIGKILL runs nothing,
so namespaces are named `<prefix>-<pid>-<lab>-n<node>`; opening a lab deletes those whose pid
is dead, and `netlab.py sweep` does the same from the shell.

**Evidence.** `python/tests/netlab_test.py --live`, as root in a Linux VM, measures each fault
between namespaces and lists namespaces, veths and tables before and after each way a harness
ends.
