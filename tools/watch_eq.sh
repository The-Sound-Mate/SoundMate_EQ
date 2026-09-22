#!/bin/bash
# watch_eq.sh - read-only live watcher.
# Pairs the song in the player's window title with the 31-band curve the
# Controller has actually published into shared memory. The SHM is opened
# FILE_MAP_READ by audit_shm_read.exe; nothing here writes to it.
# Emits one line when the song changes, or when a band group has moved
# >= 0.30 dB and at least 10 s has passed since the last line.
EXE=/c/SoundMate_EQ/tools/obj/audit_shm_read.exe
LOG=/c/SoundMate_EQ/tools/eq_watch.log
PS='powershell.exe'

# A browser can have several windows open; only one of them is the player.
# Prefer a title that looks like a media tab, and fall back to the first
# window only when nothing matches.
title_of() {
  local all
  all=$("$PS" -NoProfile -Command "[Console]::OutputEncoding=[Text.Encoding]::UTF8; Get-Process chrome,msedge,firefox,Spotify -EA 0 | Where-Object {\$_.MainWindowTitle} | ForEach-Object { \$_.MainWindowTitle }" 2>/dev/null | tr -d '\r')
  # Only a title that actually looks like a player tab counts. Chrome exposes
  # one MainWindowTitle per process and which process answers flips between
  # polls, so a non-media title means "could not see the player this time" -
  # print nothing and let the caller keep the previous song rather than
  # reporting a browser tab as a track change.
  local pick
  pick=$(printf '%s\n' "$all" | grep -m1 -E 'YouTube|Spotify|SoundCloud|Apple Music|VLC')
  [ -z "$pick" ] && return 0
  printf '%s\n' "$pick" \
    | sed -e 's/ - YouTube Music - Chrome$//' -e 's/ - YouTube - Chrome$//' \
          -e 's/ - Chrome$//' -e 's/ - YouTube Music$//' -e 's/ - YouTube$//' \
          -e 's/^([0-9]*) *//'
}

last_song=""
lb=""; ll=""; lm=""; lp=""; lt=""
last_emit=0

while true; do
  song=$(title_of)
  [ -z "$song" ] && song="$last_song"
  [ -z "$song" ] && song="(player not visible yet)"
  d=$("$EXE" 2>/dev/null | awk '
    /^updateCtr/ { ctr=$3 }
    /^ *[0-9]+ +[0-9]+ +[-+]/ { i=$1+0; g[i]=$3+0; f[i]=$2+0; n++ }
    END {
      if (n != 31) exit 1
      for (i=0;  i<=5;  i++) b+=g[i]; b/=6
      for (i=6;  i<=11; i++) l+=g[i]; l/=6
      for (i=12; i<=18; i++) m+=g[i]; m/=7
      for (i=19; i<=25; i++) p+=g[i]; p/=7
      for (i=26; i<=30; i++) t+=g[i]; t/=5
      pk=0; pi=0
      for (i=0; i<=30; i++) { a=(g[i]<0?-g[i]:g[i]); c=(pk<0?-pk:pk); if (a>c) { pk=g[i]; pi=i } }
      printf "%d %.2f %.2f %.2f %.2f %.2f %.2f %d\n", ctr, b, l, m, p, t, pk, f[pi]
    }')
  if [ -z "$d" ]; then sleep 4; continue; fi
  set -- $d
  ctr=$1; b=$2; l=$3; m=$4; p=$5; t=$6; pk=$7; pf=$8

  changed=0
  [ "$song" != "$last_song" ] && changed=2
  if [ "$changed" -eq 0 ] && [ -n "$lb" ]; then
    moved=$(awk -v a="$b" -v b="$lb" -v c="$l" -v d="$ll" -v e="$m" -v f="$lm" \
                -v g="$p" -v h="$lp" -v i="$t" -v j="$lt" \
      'BEGIN{ d1=a-b;d2=c-d;d3=e-f;d4=g-h;d5=i-j;
              if(d1<0)d1=-d1; if(d2<0)d2=-d2; if(d3<0)d3=-d3; if(d4<0)d4=-d4; if(d5<0)d5=-d5;
              print (d1>=0.30||d2>=0.30||d3>=0.30||d4>=0.30||d5>=0.30) ? 1 : 0 }')
    now=$(date +%s)
    [ "$moved" = "1" ] && [ $((now - last_emit)) -ge 10 ] && changed=1
  fi
  [ -z "$lb" ] && changed=1

  if [ "$changed" -ne 0 ]; then
    tag="     "; [ "$changed" -eq 2 ] && tag="SONG>"
    line=$(printf '%s [%s] ctr=%s bass=%+6.2f low=%+6.2f mid=%+6.2f pres=%+6.2f treb=%+6.2f peak=%+.2f@%sHz | %s' \
      "$tag" "$(date +%H:%M:%S)" "$ctr" "$b" "$l" "$m" "$p" "$t" "$pk" "$pf" "$song")
    printf '%s\n' "$line"
    printf '%s\n' "$line" >> "$LOG"
    last_emit=$(date +%s)
    lb=$b; ll=$l; lm=$m; lp=$p; lt=$t; last_song=$song
  fi
  sleep 4
done
