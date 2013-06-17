===========================================================
=========================  TOOLs  =========================
===========================================================
[01] config.pl
[02] make rebuild
[03] setting.pyw

===========================================================
######################  config.pl  ########################
-----------------------------------------------------------
Command 
	$> perl config.pl

Description
	setting net/mac80211/queueing.h defined values
	the value can be setting in queueing.ini by any 
	editor or use setting.pyw
	
	Note that to run "config.pl", Perl and its package 
	"Config::IniFiles" is needed. You can install the 
	package by following command.
	    $> perl -MCPAN -e "install 'Config::IniFiles'"

===========================================================
#####################  make rebuild  ######################
-----------------------------------------------------------
Command 
	$> make build

Description
	reconfigure and recompile the mac80211 module and 
	ath9k driver
	
	do the following instruction
	    $> perl config.pl
	    $> make
	    $> make install
	    $> make wlunload
	    $> modprobe -a ath9k
		
	Note that to run "config.pl", Perl and its package
	"Config::IniFiles" is needed. You can install the 
	package by following command.
	    $> perl -MCPAN -e "install 'Config::IniFiles'"

===========================================================
#####################  setting.pyw  #######################
-----------------------------------------------------------
Command 
	$> python setting.pyw &

Description
	an Graphic UI of "config.pl" and "make rebuild"
	
	Note that to run "setting.pyw", Python and wxPython
	is needed. 

	The installation instructions of wxPython can be 
	found in following links.
	    http://www.wxpython.org/download.php
		
	    $> apt-get install curl
	    $> curl http://apt.wxwidgets.org/key.asc \
	       | sudo apt-key add - 

	    Modify /etc/apt/source.list 
	    + deb http://apt.wxwidgets.org/ DST-wx main
	    + deb-src http://apt.wxwidgets.org/ DST-wx main
		
	    (replace DST by whatever is appropriate for
	     your system. ex:natty)

	    $> apt-get update
	    $> apt-get install python-wxgtk2.8 \
	                       python-wxtools wx2.8-i18n \ 
	                       libwxgtk2.8-dev \
	                       libgtk2.0-dev

===========================================================



===========================================================
===========================================================
== Author: 
== 	James Tsunghsiao Pan (100062587)
== 	Communications and Networking Lab
== 	Department of Computer Science
== 	National Tsing Hua University, Taiwan
===========================================================
===========================================================
