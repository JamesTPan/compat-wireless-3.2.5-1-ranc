use Config::IniFiles;
use strict;

my $cfg = Config::IniFiles->new( -file => "./queueing.ini");
my @para = $cfg->Parameters('queueing');
foreach my $para (@para){
	if(int($cfg->val('queueing', $para))){
		my $d = int($cfg->val('queueing', $para));
		print "  SET " . $para . " = $d\n";
		`sed -i 's/$para.*[0-9][0-9]*/$para\t\t$d/g' net/mac80211/queueing.h`
	}
}

my @m = ("BASIC", "FEEDBACK", "RSSI", "DISABLE", "FEEDBACK_MOD");
@para = $cfg->Parameters('relay');
foreach my $para (@para){
	if($cfg->val('relay', $para)){
		if ($para eq "method"){
			my $d = $cfg->val('relay', $para);

			if(uc($d) eq $m[3]){
				print "  SET relay_$para = $d\n";
				`sed -i 's/CONFIG_MAC80211_RELAY_DISABLE=.*/CONFIG_MAC80211_RELAY_DISABLE=y/g' config.mk`;
				`sed -i 's/CONFIG_MAC80211_RELAY_BASIC=.*/CONFIG_MAC80211_RELAY_BASIC=n/g' config.mk`;
				`sed -i 's/CONFIG_MAC80211_RELAY_FEEDBACK=.*/CONFIG_MAC80211_RELAY_FEEDBACK=n/g' config.mk`;
				`sed -i 's/CONFIG_MAC80211_RELAY_RSSI=.*/CONFIG_MAC80211_RELAY_RSSI=n/g' config.mk`;
				
				`sed -i 's/CONFIG_MAC80211_RELAY_FEEDBACK_MOD=.*/CONFIG_MAC80211_RELAY_FEEDBACK_MOD=n/g' config.mk`;
				next;
			} else {
				`sed -i 's/CONFIG_MAC80211_RELAY_DISABLE=.*/CONFIG_MAC80211_RELAY_DISABLE=n/g' config.mk`;
			}
			
			if(uc($d) eq $m[0]){
				print "  SET relay_$para = $d\n";
				`sed -i 's/CONFIG_MAC80211_RELAY_BASIC=.*/CONFIG_MAC80211_RELAY_BASIC=y/g' config.mk`;
			} else {
				`sed -i 's/CONFIG_MAC80211_RELAY_BASIC=.*/CONFIG_MAC80211_RELAY_BASIC=n/g' config.mk`;
			}

			if(uc($d) eq $m[1] || uc($d) eq $m[4]){
                                print "  SET relay_$para = $d\n";
                                `sed -i 's/CONFIG_MAC80211_RELAY_FEEDBACK=.*/CONFIG_MAC80211_RELAY_FEEDBACK=y/g' config.mk`;
                        } else {
                                `sed -i 's/CONFIG_MAC80211_RELAY_FEEDBACK=.*/CONFIG_MAC80211_RELAY_FEEDBACK=n/g' config.mk`;
                        }

			if(uc($d) eq $m[2]){
                                print "  SET relay_$para = $d\n";
                                `sed -i 's/CONFIG_MAC80211_RELAY_RSSI=.*/CONFIG_MAC80211_RELAY_RSSI=y/g' config.mk`;
                        } else {
                                `sed -i 's/CONFIG_MAC80211_RELAY_RSSI=.*/CONFIG_MAC80211_RELAY_RSSI=n/g' config.mk`;
                        }
		
			if(uc($d) eq $m[4]){
                                `sed -i 's/CONFIG_MAC80211_RELAY_FEEDBACK_MOD=.*/CONFIG_MAC80211_RELAY_FEEDBACK_MOD=y/g' config.mk`;
                        } else {
                                `sed -i 's/CONFIG_MAC80211_RELAY_FEEDBACK_MOD=.*/CONFIG_MAC80211_RELAY_FEEDBACK_MOD=n/g' config.mk`;
                        }

		}
	}
}
