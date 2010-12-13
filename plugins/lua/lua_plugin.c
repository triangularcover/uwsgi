#include "../../uwsgi.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

extern struct uwsgi_server uwsgi;

struct uwsgi_lua {
	struct lua_State **L;

	char *filename;
} ulua;

#define LONG_ARGS_LUA_BASE	17000 + (6 * 100)
#define LONG_ARGS_LUA		LONG_ARGS_LUA_BASE + 1

#define lca(L, n)		ulua_check_args(L, __FUNCTION__, n)

struct option uwsgi_lua_options[] = {

	{"lua", required_argument, 0, LONG_ARGS_LUA},

	{0, 0, 0, 0},

};

static void ulua_check_args(lua_State *L, const char *func, int n) {
	int args = lua_gettop(L);
	char error[4096];
	if (args != n) {
		if (n == 1) {
			snprintf(error, 4096, "uwsgi.%s takes 1 parameter", func+10);
		}
		else {
			snprintf(error, 4096, "uwsgi.%s takes %d parameters", func+10, n);
		}
		lua_pushstring(L, error);
        	lua_error(L);
	}
}

static int uwsgi_api_log(lua_State *L) {
	
	time_t tt;
	const char *logline ;

	lca(L, 1);

	if (lua_isstring(L, 1)) {
		logline = lua_tolstring(L, 1, NULL);
		tt = time(NULL);
        	if (logline[strlen(logline)] != '\n') {
                	uwsgi_log( UWSGI_LOGBASE " %.*s] %s\n", 24, ctime(&tt), logline);
        	}
        	else {
                	uwsgi_log( UWSGI_LOGBASE " %.*s] %s", 24, ctime(&tt), logline);
        	}
	}

	return 0;
}


static char *encode_lua_table(lua_State *L, int index, uint16_t *size) {

	char *buf, *ptrbuf;
	char *key;
	char *value;
	size_t keylen;
	size_t vallen;

	*size = 0;

	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
			key = (char *) lua_tolstring(L, -2, &keylen);
			value = (char *) lua_tolstring(L, -1, &vallen);
			if (keylen > 0xffff || vallen > 0xffff) continue;
			*size += (2+keylen+2+vallen);
		}
		lua_pop(L, 1);
	}

	buf = malloc(*size);
	if (!buf) {
		uwsgi_error("malloc()");
		exit(1);
	}

	ptrbuf = buf;
	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
			key = (char *) lua_tolstring(L, -2, &keylen);
			value = (char *) lua_tolstring(L, -1, &vallen);

			if (keylen > 0xffff || vallen > 0xffff) continue;

			*ptrbuf++ = (uint8_t) (keylen  & 0xff);
                        *ptrbuf++ = (uint8_t) ((keylen >>8) & 0xff);
			memcpy(ptrbuf, key, keylen); ptrbuf += keylen;
			*ptrbuf++ = (uint8_t) (vallen  & 0xff);
                        *ptrbuf++ = (uint8_t) ((vallen >>8) & 0xff);
			memcpy(ptrbuf, value, vallen); ptrbuf += vallen;
		}
		lua_pop(L, 1);
	}

	return buf;
}

static int uwsgi_api_cache_set(lua_State *L) {

	int args = lua_gettop(L);
        const char *key ;
        const char *value ;
        uint64_t expires = 0;
	size_t vallen;


	if (args > 1) {

		key = lua_tolstring(L, 1, NULL);
		value = lua_tolstring(L, 2, &vallen);
		if (args > 2) {
			expires = lua_tonumber(L, 3);
		}

        	uwsgi_cache_set((char *)key, strlen(key), (char *)value, (uint16_t) vallen, expires);
		
	}

	lua_pushnil(L);
	return 1;

}


static int uwsgi_api_cache_get(lua_State *L) {

        char *value ;
        uint16_t valsize;
	const char *key ;

        lca(L, 1);

	if (lua_isstring(L, 1)) {

		key = lua_tolstring(L, 1, NULL);
        	value = uwsgi_cache_get((char *)key, strlen(key), &valsize);

        	if (value) {
                	lua_pushlstring(L, value, valsize);
			return 1;
        	}

	}

	lua_pushnil(L);

        return 1;

}


static int uwsgi_api_send_message(lua_State *L) {

	int args = lua_gettop(L);
	const char *host;
	int uwsgi_fd;
	uint8_t modifier1, modifier2;
	char *pkt = NULL;
	uint16_t pktsize = 0 ;
	char buf[4096];
	int rlen;
	int items = 0;
	int input_fd = -1, timeout = -1, input_size = 0;
	
	// is this an fd ?
	if (lua_isnumber(L, 1)) {
		args = 1;
	}
	else if (lua_isstring(L, 1)) {
		host = lua_tolstring(L, 1, NULL);	
		uwsgi_fd = uwsgi_connect((char *)host, timeout, 0);
		modifier1 = lua_tonumber(L, 2);	
		modifier2 = lua_tonumber(L, 3);	
		if (args > 4) {
			timeout = lua_tonumber(L, 5);
			if (args == 7) {
				input_fd = lua_tonumber(L, 6);	
				input_size = lua_tonumber(L, 7);	
			}
		}
		if (lua_istable(L,4)) {
			// passed a table
			pkt = encode_lua_table(L, 4, &pktsize);
		}
	 	if (uwsgi_send_message(uwsgi_fd, modifier1, modifier2, pkt, pktsize, input_fd, input_size, timeout) == -1) {
			free(pkt);
			lua_pushnil(L);
			return 1;
		}
		free(pkt);

		for(;;) {
        		rlen = uwsgi_waitfd(uwsgi_fd, timeout);
        		if (rlen > 0) {
                		rlen = read(uwsgi_fd, buf, 4096);
                		if (rlen < 0) {
                        		uwsgi_error("read()");
					break;
                		}
                		else if (rlen > 0) {
					lua_pushlstring(L, buf, rlen);	
					items++;
                		}
				else {
					break;
				}
			}
        		else if (rlen == 0) {
                		uwsgi_log("uwsgi request timed out waiting for response\n");
				break;
        		}
		}

                close(uwsgi_fd);

	}
	
	return items;
}

static int uwsgi_api_cl(lua_State *L) {

	struct wsgi_request *wsgi_req = current_wsgi_req();
	
	lua_pushnumber(L, wsgi_req->post_cl);
	return 1;
}

static int uwsgi_api_req_fd(lua_State *L) {

	struct wsgi_request *wsgi_req = current_wsgi_req();
	
	lua_pushnumber(L, wsgi_req->poll.fd);
	return 1;
}

static const luaL_reg uwsgi_api[] = {
  {"log", uwsgi_api_log},
  {"cl", uwsgi_api_cl},
  {"req_fd", uwsgi_api_req_fd},
  {"send_message", uwsgi_api_send_message},
  {"cache_get", uwsgi_api_cache_get},
  {"cache_set", uwsgi_api_cache_set},
  {NULL, NULL}
};



static int uwsgi_lua_input(lua_State *L) {

	struct wsgi_request *wsgi_req = current_wsgi_req();
	ssize_t sum, len, total;
	char *buf, *ptr;

	int n = lua_gettop(L);

	if (!wsgi_req->post_cl) {
		lua_pushlstring(L, "", 0);
		return 1;
	}

	sum = lua_tonumber(L, 2);

	if (n > 1) {
		uwsgi_log("requested %d bytes\n", sum);
	}

	buf = malloc(sum);
	if (!buf) {
		uwsgi_error("malloc()");
	}

	total = sum;

	ptr = buf;
	while(total) {
		len = read(wsgi_req->poll.fd, ptr, total);
		ptr += len;
		total -= len;
	}

	lua_pushlstring(L, buf, sum);
	free(buf);

	return 1;
}

int uwsgi_lua_init(){

	uwsgi_log("Initializing Lua environment... (%d cores)\n", uwsgi.cores);

	ulua.L = malloc( sizeof(lua_State*) * uwsgi.cores );
	if (!ulua.L) {
		uwsgi_error("malloc()");
		exit(1);
	}

	// ok the lua engine is ready
	return 0;


}

void uwsgi_lua_app() {
	int i;

	if (ulua.filename) {
		for(i=0;i<uwsgi.cores;i++) {
			ulua.L[i] = luaL_newstate();
			luaL_openlibs(ulua.L[i]);
			luaL_register(ulua.L[i], "uwsgi", uwsgi_api);
			if (luaL_loadfile(ulua.L[i], ulua.filename)) {
				uwsgi_log("unable to load file %s\n", ulua.filename);
				exit(1);
			}
			// use a pcall
			//lua_call(ulua.L[i], 0, 1);
			if (lua_pcall(ulua.L[i], 0, 1, 0) != 0) {
				uwsgi_log("%s\n", lua_tostring(ulua.L[i], -1));
				exit(1);
			}
		}

	}
}

int uwsgi_lua_request(struct wsgi_request *wsgi_req) {

	int i;
	int raw;
	const char *http;
	size_t slen;
	ssize_t rlen;
	char *ptrbuf;
	lua_State *L = ulua.L[wsgi_req->async_id];

#ifdef UWSGI_ASYNC
	if (wsgi_req->async_status == UWSGI_AGAIN) {
		if ((i = lua_pcall(L, 0, 1, 0)) == 0) {
			if (lua_type(L, -1) == LUA_TSTRING) {
				http = lua_tolstring(L, -1, &slen);
				if ( (rlen = write(wsgi_req->poll.fd, http, slen)) != (ssize_t) slen) {
					perror("write()");
					return UWSGI_OK;
				}
				wsgi_req->response_size += rlen;
			}
			lua_pop(L, 1);
			lua_pushvalue(L, -1);
			return UWSGI_AGAIN;
		}
		goto clear;
	}
#endif

	/* Standard WSAPI request */
	if (!wsgi_req->uh.pktsize) {
		uwsgi_log( "Invalid WSAPI request. skip.\n");
		goto clear;
	}

	if (uwsgi_parse_vars(wsgi_req)) {
		uwsgi_log("Invalid WSAPI request. skip.\n");
		goto clear;
	}

	// put function in the stack
	//lua_getfield(L, LUA_GLOBALSINDEX, "run");
	lua_pushvalue(L, -1);

	// put cgi vars in the stack

	lua_newtable(L);
	lua_pushstring(L, "");
	lua_setfield(L, -2, "CONTENT_TYPE");
	for(i=0;i<wsgi_req->var_cnt;i++) {
		lua_pushlstring(L, (char *)wsgi_req->hvec[i+1].iov_base, wsgi_req->hvec[i+1].iov_len);
		// transform it in a valid c string TODO this is ugly
		ptrbuf = wsgi_req->hvec[i].iov_base+wsgi_req->hvec[i].iov_len;
		*ptrbuf = 0;
		lua_setfield(L, -2, (char *)wsgi_req->hvec[i].iov_base);
		i++;
	}


	// put "input" table
	lua_newtable(L);
	lua_pushcfunction(L, uwsgi_lua_input);
	lua_setfield(L, -2, "read");
	lua_setfield(L, -2, "input");


	// call function
	i = lua_pcall(L, 1, 3, 0);
	if (i != 0) {
		uwsgi_log("%s\n", lua_tostring(L, -1));
		lua_pop(L, 1);
		goto clear;
	}

	//uwsgi_log("%d %s %s %s\n",i,lua_typename(L, lua_type(L, -3)), lua_typename(L, lua_type(L, -2)) ,  lua_typename(L, lua_type(L, -1)));

	raw = 0;
	// send status
	if (lua_type(L, -3) == LUA_TSTRING || lua_type(L, -3) == LUA_TNUMBER) {
		http = lua_tolstring(L, -3, &slen);
		if (write(wsgi_req->poll.fd, wsgi_req->protocol, wsgi_req->protocol_len) != wsgi_req->protocol_len) {
			perror("write()");
			goto clear;
		}
		if (write(wsgi_req->poll.fd, " ", 1) != 1) {
			perror("write()");
			goto clear;
		}
		if (write(wsgi_req->poll.fd, http, slen) != (ssize_t) slen) {
			perror("write()");
			goto clear;
		}
		// a performance hack
		ptrbuf = (char *) http;
		ptrbuf[3] = 0;
		wsgi_req->status = atoi(ptrbuf);
		if (write(wsgi_req->poll.fd, "\r\n", 2) != 2) {
			perror("write()");
			goto clear;
		}
	}
	else {
		raw = 1;
		wsgi_req->status = -1;
	}

	// send headers

	lua_pushnil(L);
	while(lua_next(L, -3) != 0) {
		http = lua_tolstring(L, -2, &slen);
		if (write(wsgi_req->poll.fd, http, slen) != (ssize_t) slen) {
			perror("write()");
			goto clear;
		}
		if (write(wsgi_req->poll.fd, ": ", 2) != 2) {
			perror("write()");
			goto clear;
		}
		http = lua_tolstring(L, -1, &slen);
		if (write(wsgi_req->poll.fd, http, slen) != (ssize_t) slen) {
			perror("write()");
			goto clear;
		}
		if (write(wsgi_req->poll.fd, "\r\n", 2) != 2) {
			perror("write()");
			goto clear;
		}
		lua_pop(L, 1);
	}

	if (!raw) {
		if (write(wsgi_req->poll.fd, "\r\n", 2) != 2) {
			perror("write()");
			goto clear;
		}
	}

	// send body with coroutine
	lua_pushvalue(L, -1);

	while ( (i = lua_pcall(L, 0, 1, 0)) == 0) {
		if (lua_type(L, -1) == LUA_TSTRING) {
			http = lua_tolstring(L, -1, &slen);
			if ( (rlen = write(wsgi_req->poll.fd, http, slen)) != (ssize_t) slen) {
				perror("write()");
				goto clear;
			}
			wsgi_req->response_size += rlen;
		}
		lua_pop(L, 1);
		lua_pushvalue(L, -1);
#ifdef UWSGI_ASYNC
		if (uwsgi.async > 1) {
			return UWSGI_AGAIN;
		}
#endif
	}

clear:

	lua_pop(L, 4);

	// set frequency
	lua_gc(L, LUA_GCCOLLECT, 0);

	return UWSGI_OK;

}

void uwsgi_lua_after_request(struct wsgi_request *wsgi_req) {

	if (uwsgi.shared->options[UWSGI_OPTION_LOGGING])
		log_request(wsgi_req);
}

int uwsgi_lua_manage_options(int i, char *optarg) {

	switch(i) {
		case LONG_ARGS_LUA:
			ulua.filename = optarg;
			return 1;
	}

	return 0;
}

int uwsgi_lua_magic(char *mountpoint, char *lazy) {

	if (!strcmp(lazy+strlen(lazy)-4, ".lua")) {
                ulua.filename = lazy;
                return 1;
        }
        else if (!strcmp(lazy+strlen(lazy)-3, ".ws")) {
                ulua.filename = lazy;
                return 1;
        }


	return 0;
}

struct uwsgi_plugin lua_plugin = {

	.name = "lua",
	.modifier1 = 6,
	.init = uwsgi_lua_init,
	.options = uwsgi_lua_options,
	.manage_opt = uwsgi_lua_manage_options,
	.request = uwsgi_lua_request,
	.after_request = uwsgi_lua_after_request,
	.init_apps = uwsgi_lua_app,
	.magic = uwsgi_lua_magic,

};

