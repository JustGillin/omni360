#ifndef DNS_H
#define DNS_H

bool ResolveDNS(const char *domain, char *resolvedIP, int ipBufferSize);

int searchDnsCache(const char *domain, char *ip, int ipSize);

// Writes the console's network as this app sees it to the log, once: the
// cable's link, the IP address and how it was got, whether there's a gateway
// and DNS servers, and the console's clock - which HTTPS checks certificates
// against, so a wrong one fails every secure connection while plain HTTP
// still works. Waits up to a few seconds for an address, on a thread of its
// own, so it doesn't hold up starting.
void LogNetworkStatus();

// Starts the network stack once, at start, before anything connects, and
// keeps it up until the app exits. Each request still starts and stops it
// around itself, but then only counts up and down from here, rather than
// taking it down under another request running at the same time.
void StartNetwork();

#endif
