#ifndef __PD_BOOT_H__
#define __PD_BOOT_H__

const char __LUA_BOOTSTRAP[] =
"cbind = require('cbind')\n"
"cjson = require('cjson')\n"
"cutil = require('cutil')\n"
"cfile = require('cfile')\n"
"chttp = require('chttp')\n"
"clink = require('clink')\n"
"package.path = HOME .. '?.lua'"
"load(cbind.read('boot.lua', 'boot.lua'))()";

#endif //__PD_BOOT_H__
