[maxscale]
threads=###threads###
log_warning=1

[MySQL-Monitor]
type=monitor
module=mariadbmon
servers= server1, server2
user=maxskysql
password= skysql
detect_stale_master=0
monitor_interval=1000

[RW-Split-Router]
type=service
router= readwritesplit
servers=server1,     server2
user=maxskysql
password=skysql
slave_selection_criteria=LEAST_ROUTER_CONNECTIONS

[Read-Connection-Router-Slave]
type=service
router=readconnroute
router_options= slave
servers=server1,server2
user=maxskysql
password=skysql

[Read-Connection-Router-Master]
type=service
router=readconnroute
router_options=master
servers=server1,server2
user=maxskysql
password=skysql

[RW-Split-Listener]
type=listener
service=RW-Split-Router
protocol=MySQLClient
port=4006
#socket=/tmp/rwsplit.sock

[Read-Connection-Listener-Slave]
type=listener
service=Read-Connection-Router-Slave
protocol=MySQLClient
port=4009

[Read-Connection-Listener-Master]
type=listener
service=Read-Connection-Router-Master
protocol=MySQLClient
port=4008

[CLI]
type=service
router=cli

[CLI-Listener]
type=listener
service=CLI
protocol=maxscaled
#address=localhost
socket=default

[server1]
type=server
address=###node_server_IP_1###
port=###node_server_port_1###
protocol=MySQLBackend

[server2]
type=server
address=###node_server_IP_2###
port=###node_server_port_2###
protocol=MySQLBackend

