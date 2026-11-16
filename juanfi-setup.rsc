:put "=== JuanFiV2 setup ==="

:do { /system script remove [find name="juanfi-setup"] } on-error={};
:do { /system scheduler remove [find name="juanfi-setup-daily"] } on-error={};

:do {
  /system script add name="juanfi-setup" policy=read,write,test,policy,ftp source={
    :local PROF "hsprof1";
    :local NTP1 "216.239.35.8";
    :local NTP2 "216.239.35.4";
    :local HSFilePath "";
    :do { :set HSFilePath [/ip hotspot profile get [find name=$PROF] html-directory] } on-error={ :set HSFilePath "" };
    :if (($HSFilePath = "") or ([/file find name=($HSFilePath . "/portal.html")] = "")) do={
      :local cand "";
      :foreach c in={"flash/hotspot"; "hotspot"} do={
        :if ($cand = "") do={
          :foreach f in={"portal.html"; "login.html"; "status.html"} do={
            :if (($cand = "") and ([/file find name=($c . "/" . $f)] != "")) do={ :set cand $c };
          };
        };
      };
      :if ($cand != "") do={ :set HSFilePath $cand };
    };
    :if ($HSFilePath = "") do={ :set HSFilePath "hotspot" };
    :local dataDir ($HSFilePath . "/data");
    :put ("portal files = " . $HSFilePath);
    :if ([/file find name=($HSFilePath . "/portal.html")] = "") do={ :log warning ("setup: no portal.html under " . $HSFilePath) };
    :if ([/file find name=($dataDir . "/.keep")] = "") do={
      :do { /file print file=($dataDir . "/.keep") where name="dummyfile" } on-error={};
      :local x 3;
      :while (($x>0) and ([/file find name=($dataDir . "/.keep")] = "")) do={ :set x ($x-1); :delay 1s };
      :do { /file remove ($dataDir . "/.keep") } on-error={};
    };
    :local rosV "";
    :do { :set rosV [:pick [/system resource get version] 0 1] } on-error={ :set rosV "" };
    :local ntpCmd "";
    :if ($rosV = "7") do={ :set ntpCmd ("/system ntp client set enabled=yes servers=" . $NTP1 . "," . $NTP2) } else={ :set ntpCmd ("/system ntp client set enabled=yes primary-ntp=" . $NTP1 . " secondary-ntp=" . $NTP2) };
    :do { :local ntpFn [:parse $ntpCmd]; $ntpFn } on-error={ :log warning "setup: NTP not configurable" };
    :do { /system clock set time-zone-name=Asia/Manila } on-error={};
    :do { /ip hotspot profile set [find name=$PROF] login-by=http-chap,http-pap } on-error={ :log warning ("setup: no hotspot profile named " . $PROF) };
    :do { /ip hotspot user profile set [find name="default"] idle-timeout=none keepalive-timeout=30s status-autorefresh=1m } on-error={};
    :if ([:len [/ip firewall filter find comment="hotspot before fasttrack"]] = 0) do={
      :do { /ip firewall filter add chain=forward action=accept protocol=tcp dst-port=80,443 src-address=10.0.0.0/16 comment="hotspot before fasttrack" } on-error={ :log warning "setup: fasttrack rule not added" };
      :do { /ip firewall filter move [find comment="hotspot before fasttrack"] destination=0 } on-error={};
    };
    :local siteFile ($dataDir . "/site-id.txt");
    :local siteOld "";
    :do { :set siteOld [/file get [find name=$siteFile] contents] } on-error={ :set siteOld "" };
    :if ($siteOld = "") do={
      :local sn "";
      :do { :set sn [/system routerboard get serial-number] } on-error={};
      :if ([:len $sn] < 4) do={ :set sn $PROF };
      :do { /file print file=$siteFile where name="dummyfile" } on-error={};
      :local y 5;
      :while (($y>0) and ([/file find name=$siteFile] = "")) do={ :set y ($y-1); :delay 1s };
      :do { /file set $siteFile contents=$sn } on-error={ :log warning "setup: site-id not written" };
    };
  };
} on-error={ :log warning "setup: juanfi-setup not created" };

:do { /system script run juanfi-setup } on-error={ :log warning "setup: juanfi-setup run failed" };

:put "=== RouterOS API for the coin box ===";
:do { /ip service enable api } on-error={ :log warning "setup: api service not enabled" };
:do { /ip service set api port=8728 } on-error={ :log warning "setup: api port not set" };
:if ([:len [/user group find name="juanfi-api"]] = 0) do={
  :do { /user group add name="juanfi-api" policy=api,read,write comment="ESP coin box (RouterOS API)" } on-error={ :log warning "setup: juanfi-api group not created" };
};
:if ([:len [/ip firewall filter find comment="juanfi api from hotspots"]] = 0) do={
  :do { /ip firewall filter add chain=input action=accept protocol=tcp dst-port=8728 src-address=10.0.0.0/16 comment="juanfi api from hotspots" } on-error={ :log warning "setup: api input rule not added" };
  :do { /ip firewall filter move [find comment="juanfi api from hotspots"] destination=0 } on-error={};
};
:local apiUserCount 0;
:foreach u in=[/user find] do={ :do { :if ([/user get $u group] = "juanfi-api") do={ :set apiUserCount ($apiUserCount + 1) } } on-error={} };
:if ($apiUserCount = 0) do={ :put "WARNING: no user in group juanfi-api - create one and enter the same name/password in the ESP admin page"; :log warning "setup: no user in group juanfi-api yet" } else={ :put ("juanfi-api users = " . $apiUserCount) };

:do { /system scheduler add name="juanfi-setup-daily" start-time=startup interval=1d policy=read,write,test,policy,ftp on-event="/system script run juanfi-setup" } on-error={ :log warning "setup: juanfi-setup-daily not added" };

:put "=== done, verifying ===";
:put [/system clock get date-time];
:local PROF "hsprof1";
:local VPath "";
:do { :set VPath [/ip hotspot profile get [find name=$PROF] html-directory] } on-error={ :set VPath "" };
:if (($VPath = "") or ([/file find name=($VPath . "/portal.html")] = "")) do={
  :local cand "";
  :foreach c in={"flash/hotspot"; "hotspot"} do={
    :if ($cand = "") do={
      :foreach f in={"portal.html"; "login.html"; "status.html"} do={
        :if (($cand = "") and ([/file find name=($c . "/" . $f)] != "")) do={ :set cand $c };
      };
    };
  };
  :if ($cand != "") do={ :set VPath $cand };
};
:if ($VPath = "") do={ :set VPath "hotspot" };
:put ("portal files = " . $VPath);
:do { :put ("site-id  = " . [/file get [find name=($VPath . "/data/site-id.txt")] contents]) } on-error={ :put "site-id  = MISSING" };
:put "=== juanfi-setup done ===";
