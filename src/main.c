#include "wi-fi.h"
#include "web_server.h"
void app_main()
{
    wifi_init();
    start_web_server();
}