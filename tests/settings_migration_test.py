"""Host regression checks for firmware settings validation and migration.

Run from any directory with Python 3 and gcc installed.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import re, subprocess
s=(Path(__file__).resolve().parents[1] / 'src/firmware.c').read_text()
def function(name):
 m=re.search(r'^static [\w *]+\b'+name+r'\s*\(',s,re.M)
 start=s.index('{',m.end());depth=0
 for i in range(start,len(s)):
  if s[i]=='{':depth+=1
  elif s[i]=='}':
   depth-=1
   if depth==0:return s[m.start():i+1]
structs='\n'.join(m.group() for m in re.finditer(r'typedef struct \{.*?\} (\w+);',s,re.S) if m[1] in ['display_style_t','persisted_settings_t','persisted_settings_v3_t','persisted_settings_v2_t','persisted_settings_v1_t'])
a=s.rfind('enum {',0,s.index('DISPLAY_FLAG_BORDER_ENABLED ='));b=s.index('};',a)+2
prefix='''#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
enum { SETTINGS_MAGIC=0x41473256, SETTINGS_VERSION=6, SETTINGS_SLOT_COUNT=2,
 P2000M_PHOSPHOR_NOISE_LEVEL_COUNT=4, P2000M_PHOSPHOR_NOISE_OFF=0 };
'''+s[a:b]+'\n'+structs+'''
static persisted_settings_t slots[2];
static int saved_settings_slot, saved_manual_phase_ticks;
static uint32_t saved_settings_sequence;
static unsigned sync_value, demo_value;
static bool delay_value;
static const persisted_settings_t *settings_slot_record(unsigned i) {return &slots[i];}
static void pal_output_set_sync_advance(unsigned v) {sync_value=v;if(v>=6)delay_value=false;}
static void pal_output_set_picture_delay(bool v) {delay_value=v;}
static void pal_output_set_demo(unsigned v) {demo_value=v;}
'''
functions='\n'.join(function(n) for n in ['settings_crc32','settings_record_is_valid','settings_v3_record_is_valid','settings_v2_record_is_valid','settings_v1_record_is_valid','settings_sequence_is_newer','load_saved_configuration'])
test=r'''
static void seal(persisted_settings_t *r) {r->checksum=settings_crc32(r,28);}
static void clear(void) {memset(slots,0xff,sizeof(slots));sync_value=0;demo_value=0;delay_value=true;saved_settings_slot=-1;}
int main(void) {
 assert(sizeof(persisted_settings_t)==32 && offsetof(persisted_settings_t,checksum)==28);
 persisted_settings_t r={.magic=SETTINGS_MAGIC,.version=5,.length=32,.sequence=2,
 .foreground_rgb=0x123456,.background_rgb=0x112233,.border_rgb=0xabcdef,
 .display_flags=7,.phosphor_noise_level=2,.manual_phase_ticks=-3};
 for(unsigned version=4;version<=6;version++) for(unsigned flags=0;flags<256;flags++) {
  r.version=version;r.output_flags=flags;seal(&r);
  bool valid=version==4 ? flags<4 : (version==6 || flags<64) && !(((flags>>3)&7)>=6 && (flags&4));
  assert(settings_record_is_valid(&r)==valid);
  if(valid) {
   clear();slots[0]=r;display_style_t style={0};bool vga=true,pal=true;
   assert(load_saved_configuration(&style,&vga,&pal));
   assert(style.foreground_rgb==0x123456 && style.background_rgb==0x112233);
   assert(style.border_rgb==0xabcdef && style.border_enabled && style.border_dotted);
   assert(style.vertical_stretch_enabled && style.phosphor_noise_level==2);
   assert(saved_manual_phase_ticks==-3 && saved_settings_sequence==2);
   assert(vga==!!(flags&1) && pal==!!(flags&2));
   assert(demo_value==(version==6 ? flags>>6 : 0));
   assert(sync_value==(version>=5 ? (flags>>3)&7 : 0));
   assert(delay_value==(version>=5 ? !!(flags&4) : true));
  }
 }
 // Old v3 values migrate without reinterpreting its reserved byte as timing.
 clear();r.version=3;r.output_flags=255;seal(&r);slots[0]=r;
 display_style_t style={0};bool vga=true,pal=true;
 assert(load_saved_configuration(&style,&vga,&pal));
 assert(style.phosphor_noise_level==2 && sync_value==0 && delay_value);
 assert(vga && pal);
 // Legacy v2 and v1 keep their historical layouts and do not change PAL timing.
 clear();persisted_settings_v2_t v2={.magic=SETTINGS_MAGIC,.version=2,.length=32,.sequence=3,
 .foreground_rgb=0x123456,.background_rgb=0x112233,.border_rgb=0xabcdef,
 .border_enabled=1,.border_dotted=1,.vertical_stretch_enabled=1,.manual_phase_ticks=4};
 v2.checksum=settings_crc32(&v2,28);memcpy(&slots[0],&v2,32);
 assert(load_saved_configuration(&style,&vga,&pal));
 assert(style.border_rgb==0xabcdef && saved_manual_phase_ticks==4 && delay_value && sync_value==0);
 clear();persisted_settings_v1_t v1={.magic=SETTINGS_MAGIC,.version=1,.length=32,.sequence=4,
 .foreground_rgb=0x123456,.background_rgb=0x112233,.border_enabled=1,
 .vertical_stretch_enabled=1,.manual_phase_ticks=-4};
 v1.checksum=settings_crc32(&v1,28);memcpy(&slots[0],&v1,32);
 assert(load_saved_configuration(&style,&vga,&pal));
 assert(style.border_rgb==0x123456 && !style.border_dotted && saved_manual_phase_ticks==-4);
 // A damaged newer record must fall back to the previous valid slot.
 clear();r.version=4;r.output_flags=3;r.sequence=10;seal(&r);slots[0]=r;
 r.version=5;r.sequence=11;r.output_flags=59;seal(&r);slots[1]=r;
 assert(load_saved_configuration(&style,&vga,&pal) && sync_value==7 && !delay_value);
 slots[1].checksum^=1;sync_value=0;delay_value=true;
 assert(load_saved_configuration(&style,&vga,&pal) && saved_settings_slot==0 && sync_value==0 && delay_value);
 // CRC-valid but contradictory timing is rejected too.
 r.output_flags=63;seal(&r);slots[1]=r;
 assert(load_saved_configuration(&style,&vga,&pal) && saved_settings_slot==0);
 puts("PASS: v1-v5 migration, all v4/v5/v6 flags, artwork/timing restore, CRC and fallback");
}
'''
with TemporaryDirectory(prefix='p2000m-settings-') as directory:
 p=Path(directory)/'settings_test.c'
 binary=Path(directory)/'settings_test'
 p.write_text(prefix+functions+test)
 subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
