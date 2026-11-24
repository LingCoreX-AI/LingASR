#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { ASSISTANT_WIFI_MAX=8, ASSISTANT_WIFI_SCAN_MAX=32 };
typedef struct { char ssid[33],password[65]; uint8_t hidden; } assistant_wifi_profile_t;
typedef struct { uint32_t version,count; char preferred[33]; assistant_wifi_profile_t profiles[ASSISTANT_WIFI_MAX]; } assistant_wifi_store_t;
typedef struct { char ssid[33]; int rssi; bool open; } assistant_wifi_ap_t;
void assistant_wifi_init(assistant_wifi_store_t *store);
bool assistant_wifi_valid(const assistant_wifi_store_t *store);
int assistant_wifi_find(const assistant_wifi_store_t *store,const char *ssid);
bool assistant_wifi_save(assistant_wifi_store_t *store,const char *ssid,const char *password,bool hidden);
bool assistant_wifi_remove(assistant_wifi_store_t *store,const char *ssid);
/* Preferred visible, strongest other visible, marked-hidden, then preferred by name. */
int assistant_wifi_choose(const assistant_wifi_store_t *store,const assistant_wifi_ap_t *aps,size_t count,unsigned tried);
