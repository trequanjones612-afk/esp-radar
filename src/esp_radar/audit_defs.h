// AUTO: audit type/table definitions (kept in a header so Arduino auto-prototypes resolve the types)
#pragma once
#include <Arduino.h>
#include <WiFi.h>

struct PortDef { uint16_t port; const char *svc; uint8_t kind; };
// kind: 0 generic, 1 http, 2 https, 3 banner, 4 rtsp, 5 smb, 6 telnet, 7 rdp, 8 tr069, 9 mqtt, 10 ftp, 11 msrpc
static const PortDef AUDIT_PORTS[] = {
  {21,"FTP",10},{22,"SSH",3},{23,"Telnet",6},{25,"SMTP",3},{53,"DNS",0},{80,"HTTP",1},{110,"POP3",3},
  {135,"MSRPC",11},{139,"NetBIOS-SSN",5},{143,"IMAP",3},{443,"HTTPS",2},{445,"SMB",5},{554,"RTSP",4},
  {587,"SMTP-sub",3},{1883,"MQTT",9},{2323,"Telnet-alt",6},{3389,"RDP",7},{5000,"HTTP/UPnP",1},
  {5357,"WSD",0},{7547,"TR-069",8},{8000,"HTTP-cam",1},{8080,"HTTP-alt",1},{8081,"HTTP-alt",1},
  {8443,"HTTPS-alt",2},{8554,"RTSP-alt",4},{9000,"HTTP-alt",1},{37777,"Dahua",0},{49152,"UPnP",1},{32764,"backdoor?",0},
};
static const int AUDIT_NPORTS = sizeof(AUDIT_PORTS) / sizeof(AUDIT_PORTS[0]);

// Capped list of universally-known factory defaults (NOT a brute-forcer): 7 universal + up to 1 vendor-specific = max 8.
struct Cred { const char *u, *p; };
static const Cred DEF_CREDS[] = {
  {"admin","admin"},{"admin","password"},{"admin",""},{"root","root"},{"user","user"},{"admin","1234"},{"support","support"},
};
static const int DEF_CREDS_N = sizeof(DEF_CREDS) / sizeof(DEF_CREDS[0]);   // 7; a vendor default may add 1 (max 8)

struct VerHint { const char *needle; uint8_t sev; const char *note; };
static const VerHint VER_HINTS[] = {
  {"OpenSSH_6",1,"Very old OpenSSH"},{"OpenSSH_7.",1,"Old OpenSSH"},{"Boa/",2,"Boa webserver - common on old IP cameras, unmaintained for years"},
  {"GoAhead",2,"GoAhead embedded webserver - common on cheap IP cameras with known issues"},{"RomPager",3,"RomPager stack - 'Misfortune Cookie' era router firmware"},
  {"thttpd/2.2",1,"Old thttpd"},{"mini_httpd",1,"Old mini_httpd"},{"lighttpd/1.4.2",1,"Old lighttpd 1.4.2x"},{"Hikvision",2,"Hikvision device web service"},
};
static const int VER_HINTS_N = sizeof(VER_HINTS) / sizeof(VER_HINTS[0]);

struct Audit {
  volatile bool running = false, cancel = false, def = true;
  uint8_t curIp = 0;
  int done = 0, total = 0, scanned = 0;
  char phase[48] = "";
  uint8_t queue[80]; int qh = 0, qt = 0;
} A;

struct ACache { bool used = false; uint8_t mac[6]; uint8_t sev; uint32_t when; String json; };
