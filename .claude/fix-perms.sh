#!/bin/bash
#
# fix permissions from sudo make install, accidentally creating build artifacts
#

for proj in treebrowser geanycli geanyagent
do
    sudo chown -R teknopaul:teknopaul $proj/src
done

