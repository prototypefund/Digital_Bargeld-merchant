#!/bin/bash

echo -e "\nThis script will set and show all the configuration\n\
values needed to run an example backend. The backend will listen on port 8888\n\
and cooperate with exchange at https://exchange.demo.taler.net/.\n\
Additionally, the script will also generate the backend's private\n\
key and banking details' file.\n\n"

echo -n "Press ENTER to start > "
read
echo 

echo -n "Setting section [merchant]<ENTER> "
read
echo 

echo -n "taler-config -s merchant -o serve -V TCP<ENTER> "
read
echo 
taler-config -s merchant -o serve -V TCP

echo -n "taler-config -s merchant -o port -V 8888<ENTER> "
read
echo 
taler-config -s merchant -o port -V 8888

echo -n "taler-config -s merchant -o database -V postgres<ENTER> "
read
echo 
taler-config -s merchant -o database -V postgres

echo -n "taler-config -s merchant -o currency -V KUDOS<ENTER> "
read
echo 
taler-config -s merchant -o currency -V KUDOS

echo -n "taler-config -s merchant -o wireformat -V TEST<ENTER> "
read
echo 
taler-config -s merchant -o wireformat -V TEST

echo -n "Setting section [merchant-instance-default]<ENTER> "
read
echo 

echo -n "taler-config -s merchant-instance-default -o keyfile -V \${TALER_DATA_HOME}/key.priv<ENTER> "
read
echo 
taler-config -s merchant-instance-default -o keyfile -V ${TALER_DATA_HOME}/key.priv


echo -n "Setting section [merchant-instance-wireformat-default]<ENTER> "
read
echo 

echo -n "taler-config -s merchant-instance-wireformat-default -o test_response_file -V \${TALER_DATA_HOME}/test.json<ENTER> "
read
echo 
taler-config -s merchant-instance-wireformat-default -o test_response_file -V ${TALER_DATA_HOME}/test.json

echo -n "Setting section [merchantdb-postgres]<ENTER> "
read
echo 

echo -n "taler-config -s merchantdb-postgres -o config -V postgres:///donations<ENTER> "
read
echo 
taler-config -s merchantdb-postgres -o config -V "postgres:///donations"

echo -n "Setting section [merchant-demoexchange]<ENTER> "
read
echo 

echo -n "taler-config -s merchant-demoexchange -o uri -V https://exchange.demo.taler.net/<ENTER> "
read
echo 
taler-config -s merchant-demoexchange -o uri -V "https://exchange.demo.taler.net/"

echo -n "taler-config -s merchant-demoexchange -o master_key -V CQQZ9DY3MZ1ARMN5K1VKDETS04Y2QCKMMCFHZSWJWWVN82BTTH00<ENTER> "
read
echo 
taler-config -s merchant-demoexchange -o master_key -V "CQQZ9DY3MZ1ARMN5K1VKDETS04Y2QCKMMCFHZSWJWWVN82BTTH00"

# FIXME:
#       1) put banking details generator
#       2) test!
