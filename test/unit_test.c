#include "mongoose_ex.c"

#define FATAL(str, line) do {                                               \
  printf("Fail on line %d: [%s]\n", line, str);                             \
  mg_signal_stop(ctx);														\
  abort();																	\
} while (0)

#define ASSERT(expr)                                                        \
    do {                                                                    \
      if (!(expr)) {                                                        \
        FATAL(#expr, __LINE__);                                             \
      }                                                                     \
    } while (0)

#define ASSERT_STREQ(str1, str2)                                            \
    do {                                                                    \
      if (strcmp(str1, str2)) {                                             \
        printf("Fail on line %d: strings not matching: "                    \
               "inp:\"%s\" != ref:\"%s\"\n",                                \
               __LINE__, str1, str2);                                       \
		mg_signal_stop(ctx);												\
        /* abort(); */                                                      \
      }                                                                     \
    } while (0)

static void test_parse_http_request() {
  struct mg_context ctx_fake = {0};
  struct mg_context *ctx = &ctx_fake;
  struct mg_request_info ri;
  char req1[] = "GET / HTTP/1.1\r\n\r\n";
  char req2[] = "BLAH / HTTP/1.1\r\n\r\n";
  char req3[] = "GET / HTTP/1.1\r\nBah\r\n";

  ASSERT(parse_http_request(req1, &ri) == 1);
  ASSERT_STREQ(ri.http_version, "1.1");
  ASSERT(ri.num_headers == 0);

  ASSERT(parse_http_request(req2, &ri) == 0);

  // TODO(lsm): Fix this. Bah is not a valid header.
  ASSERT(parse_http_request(req3, &ri) == 1);
  ASSERT(ri.num_headers == 1);
  ASSERT_STREQ(ri.http_headers[0].name, "Bah\r\n");

  // TODO(lsm): add more tests.
}

static void test_should_keep_alive(void) {
  struct mg_context ctx_fake = {0};
  struct mg_context *ctx = &ctx_fake;
  struct mg_connection conn;
  char req1[] = "GET / HTTP/1.1\r\n\r\n";
  char req2[] = "GET / HTTP/1.0\r\n\r\n";
  char req3[] = "GET / HTTP/1.1\r\nConnection: close\r\n\r\n";
  char req4[] = "GET / HTTP/1.1\r\nConnection: keep-alive\r\n\r\n";

  memset(&conn, 0, sizeof(conn));
  conn.ctx = ctx;
  parse_http_request(req1, &conn.request_info);
  conn.request_info.status_code = 200;

  ctx->config[ENABLE_KEEP_ALIVE] = "no";
  ASSERT(should_keep_alive(&conn) == 0);

  ctx->config[ENABLE_KEEP_ALIVE] = "yes";
  ASSERT(should_keep_alive(&conn) == 1);

  conn.must_close = 1;
  ASSERT(should_keep_alive(&conn) == 0);

  conn.must_close = 0;
  parse_http_request(req2, &conn.request_info);
  ASSERT(should_keep_alive(&conn) == 0);

  parse_http_request(req3, &conn.request_info);
  ASSERT(should_keep_alive(&conn) == 0);

  parse_http_request(req4, &conn.request_info);
  ASSERT(should_keep_alive(&conn) == 1);

  conn.request_info.status_code = 401;
  ASSERT(should_keep_alive(&conn) == 0);

  conn.request_info.status_code = 500;
  ASSERT(should_keep_alive(&conn) == 0);

  conn.request_info.status_code = 302;
  ASSERT(should_keep_alive(&conn) == 1);

  conn.request_info.status_code = 200;
  conn.must_close = 1;
  ASSERT(should_keep_alive(&conn) == 0);
}

static void test_match_prefix(void) {
  struct mg_context ctx_fake = {0};
  struct mg_context *ctx = &ctx_fake;

  ASSERT(match_prefix("/api", 4, "/api") == 4);
  ASSERT(match_prefix("/a/", 3, "/a/b/c") == 3);
  ASSERT(match_prefix("/a/", 3, "/ab/c") == -1);
  ASSERT(match_prefix("/*/", 3, "/ab/c") == 4);
  ASSERT(match_prefix("**", 2, "/a/b/c") == 6);
  ASSERT(match_prefix("/*", 2, "/a/b/c") == 2);
  ASSERT(match_prefix("*/*", 3, "/a/b/c") == 2);
  ASSERT(match_prefix("**/", 3, "/a/b/c") == 5);
  ASSERT(match_prefix("**.foo|**.bar", 13, "a.bar") == 5);
  ASSERT(match_prefix("a|b|cd", 6, "cdef") == 2);
  ASSERT(match_prefix("a|b|c?", 6, "cdef") == 2);
  ASSERT(match_prefix("a|?|cd", 6, "cdef") == 1);
  ASSERT(match_prefix("/a/**.cgi", 9, "/foo/bar/x.cgi") == -1);
  ASSERT(match_prefix("/a/**.cgi", 9, "/a/bar/x.cgi") == 12);
  ASSERT(match_prefix("**/", 3, "/a/b/c") == 5);
  ASSERT(match_prefix("**/$", 4, "/a/b/c") == -1);
  ASSERT(match_prefix("**/$", 4, "/a/b/") == 5);
  ASSERT(match_prefix("$", 1, "") == 0);
  ASSERT(match_prefix("$", 1, "x") == -1);
  ASSERT(match_prefix("*$", 2, "x") == 1);
  ASSERT(match_prefix("/$", 2, "/") == 1);
  ASSERT(match_prefix("**/$", 4, "/a/b/c") == -1);
  ASSERT(match_prefix("**/$", 4, "/a/b/") == 5);
  ASSERT(match_prefix("*", 1, "/hello/") == 0);
  ASSERT(match_prefix("**.a$|**.b$", 11, "/a/b.b/") == -1);
  ASSERT(match_prefix("**.a$|**.b$", 11, "/a/b.b") == 6);
  ASSERT(match_prefix("**.a$|**.b$", 11, "/a/b.a") == 6);
}

static void test_remove_double_dots() {
  struct { char before[20], after[20]; } data[] = {
    {"////a", "/a"},
    {"/.....", "/."},
    {"/......", "/"},
    {"...", "..."},
    {"/...///", "/./"},
    {"/a...///", "/a.../"},
    {"/.x", "/.x"},
    {"/\\", "/"},    /* as we have cross-platform code/storage, we do NOT accept the '/' as part of any filename, even in UNIX! */
    {"/a\\", "/a\\"},
  };
  size_t i;
  struct mg_context ctx_fake = {0};
  struct mg_context *ctx = &ctx_fake;

  for (i = 0; i < ARRAY_SIZE(data); i++) {
    //printf("[%s] -> [%s]\n", data[i].before, data[i].after);
    remove_double_dots_and_double_slashes(data[i].before);
    ASSERT_STREQ(data[i].before, data[i].after);
  }
}

static void test_IPaddr_parsing() {
  struct usa sa;
  struct vec v;
  struct socket s;
  struct mg_context ctx_fake = {0};
  struct mg_context *ctx = &ctx_fake;

    memset(&sa, 0, sizeof(sa));
    ASSERT(!parse_ipvX_addr_string("example.com", 80, &sa));
    ASSERT(parse_ipvX_addr_string("10.11.12.13", 80, &sa));
    ASSERT(sa.u.sa.sa_family == AF_INET);
    ASSERT(sa.u.sa.sa_data[0] == 0);
    ASSERT(sa.u.sa.sa_data[1] == 80);
    ASSERT(sa.u.sa.sa_data[2] == 10);
    ASSERT(sa.u.sa.sa_data[3] == 11);
    ASSERT(sa.u.sa.sa_data[4] == 12);
    ASSERT(sa.u.sa.sa_data[5] == 13);

    memset(&s, 0, sizeof(s));
    memset(&v, 0, sizeof(v));
    v.ptr = "example.com:80";
    v.len = strlen(v.ptr);
    ASSERT(!parse_port_string(&v, &s));
    v.ptr = ":80";
    v.len = strlen(v.ptr);
    ASSERT(!parse_port_string(&v, &s));
    v.ptr = "80";
    v.len = strlen(v.ptr);
    ASSERT(parse_port_string(&v, &s));
    ASSERT(s.lsa.u.sin.sin_port == htons(80));
    ASSERT(!s.is_ssl);
    v.ptr = "443s";
    v.len = strlen(v.ptr);
    ASSERT(parse_port_string(&v, &s));
    ASSERT(s.lsa.u.sin.sin_port == htons(443));
    ASSERT(s.is_ssl);
    v.ptr = "10.11.12.13:80";
    v.len = strlen(v.ptr);
    ASSERT(parse_port_string(&v, &s));
    ASSERT(s.lsa.u.sa.sa_family == AF_INET);
    ASSERT(s.lsa.u.sa.sa_data[0] == 0);
    ASSERT(s.lsa.u.sa.sa_data[1] == 80);
    ASSERT(s.lsa.u.sa.sa_data[2] == 10);
    ASSERT(s.lsa.u.sa.sa_data[3] == 11);
    ASSERT(s.lsa.u.sa.sa_data[4] == 12);
    ASSERT(s.lsa.u.sa.sa_data[5] == 13);
    ASSERT(!s.is_ssl);

    v.ptr = "  20.21.22.23:280s,  ";
    v.len = strlen(v.ptr);
    ASSERT(parse_port_string(&v, &s));
    ASSERT(s.lsa.u.sa.sa_family == AF_INET);
    ASSERT(s.lsa.u.sa.sa_data[0] == 280 / 256);
    ASSERT(s.lsa.u.sa.sa_data[1] == 280 % 256);
    ASSERT(s.lsa.u.sa.sa_data[2] == 20);
    ASSERT(s.lsa.u.sa.sa_data[3] == 21);
    ASSERT(s.lsa.u.sa.sa_data[4] == 22);
    ASSERT(s.lsa.u.sa.sa_data[5] == 23);
    ASSERT(s.is_ssl);

    v.ptr = "[10.11.12.13]:180     ,    ";
    v.len = strlen(v.ptr);
    ASSERT(parse_port_string(&v, &s));
    ASSERT(s.lsa.u.sa.sa_family == AF_INET);
    ASSERT(s.lsa.u.sa.sa_data[0] == 0);
    ASSERT(s.lsa.u.sa.sa_data[1] == (char)180);
    ASSERT(s.lsa.u.sa.sa_data[2] == 10);
    ASSERT(s.lsa.u.sa.sa_data[3] == 11);
    ASSERT(s.lsa.u.sa.sa_data[4] == 12);
    ASSERT(s.lsa.u.sa.sa_data[5] == 13);
    ASSERT(!s.is_ssl);

    v.ptr = "80  ,  ";
    v.len = strlen(v.ptr);
    ASSERT(parse_port_string(&v, &s));
    ASSERT(s.lsa.u.sin.sin_port == htons(80));
    ASSERT(!s.is_ssl);
    v.ptr = "443s  ,  ";
    v.len = strlen(v.ptr);
    ASSERT(parse_port_string(&v, &s));
    ASSERT(s.lsa.u.sin.sin_port == htons(443));
    ASSERT(s.is_ssl);


    // TODO: test these:
    //  check_acl()
    //  parse_ipvX_addr_and_netmask()

}

static void test_logpath_fmt() {
  char *uri_input[] = {
    "http://example.com/Oops.I.did.it.again....yeah....yeah....yeah....errr....ohhhhh....you shouldn't have.... Now let's see whether this bugger does da right thang for long URLs when we wanna have them as part of the logpath..........",
    "http://example.com/Oops.I.did.it.again....yeah....yeah....yeah....errr....?&_&_&_&_&_ohhhhh....you shouldn't have.... Now let's see whether this bugger does da right thang for long URLs when we wanna have them as part of the logpath..........",
    "http://example.com/sample/page/tree?with-query=y&oh%20baby,%20oops!%20I%20did%20it%20again!"
  };
  const char *path;
  char buf[512];
  char tinybuf[13];
  struct mg_context ctx_fake = {0};
  struct mg_context *ctx = &ctx_fake;
  struct mg_connection c;
  c.ctx = ctx;

    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    path = mg_get_logfile_path(tinybuf, sizeof(tinybuf), "%[U].long-blubber.log", &c, time(NULL));
    ASSERT(path);
    ASSERT_STREQ(path, "_.long-blubb");

    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[2];
    path = mg_get_logfile_path(tinybuf, sizeof(tinybuf), "%[U].long-blubber.log", &c, time(NULL));
    ASSERT(path);
    ASSERT_STREQ(path, "http.example");

    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[0];
    path = mg_get_logfile_path(tinybuf, sizeof(tinybuf), "%Y/%[U]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "_Y/http.exam");

    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[0];
    path = mg_get_logfile_path(tinybuf, sizeof(tinybuf), "%Y/%[Q]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "_Y/_/_d/_m/b");


    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = NULL;
    path = mg_get_logfile_path(buf, sizeof(buf), "%[U].long-blubber.log", &c, time(NULL));
    ASSERT(path);
    ASSERT_STREQ(path, "_.long-blubber.log");

    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[2];
    path = mg_get_logfile_path(buf, sizeof(buf), "%[U].long-blubber.log", &c, time(NULL));
    ASSERT(path);
    ASSERT_STREQ(path, "http.example.com.sample.page.tree.long-blubber.log");

    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[0];
    path = mg_get_logfile_path(buf, sizeof(buf), "%Y/%[U]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "2009/http.example.com.Oops.I.did.it.again.yeah.yeah.yeah.errr396693b9/14/02/blubber.log");

    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[1];
    path = mg_get_logfile_path(buf, sizeof(buf), "%Y/%[Q]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "2009/_ohhhhh.you_shouldn_t_have._Now_let_s_see_whether_this_bd2d6cc07/14/02/blubber.log");

    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[0];
    path = mg_get_logfile_path(buf, sizeof(buf), "%Y/%[Q]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "2009/_/14/02/blubber.log");

    // check the %[nX] numeric size parameter for %[Q/U] path formatters:

    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[1];
    path = mg_get_logfile_path(buf, sizeof(buf), "%Y/%[20Q]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "2009/_ohhhhh.you_d2d6cc07/14/02/blubber.log");

    // invalid; ignore
    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[1];
    path = mg_get_logfile_path(buf, sizeof(buf), "%Y/%[0Q]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "2009/_ohhhhh.you_shouldn_t_have._Now_let_s_see_whether_this_bd2d6cc07/14/02/blubber.log");

    // invalid, ignore
    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[1];
    path = mg_get_logfile_path(buf, sizeof(buf), "%Y/%[-5Q]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "2009/_ohhhhh.you_shouldn_t_have._Now_let_s_see_whether_this_bd2d6cc07/14/02/blubber.log");

    // very tiny; crunch the hash
    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[1];
    path = mg_get_logfile_path(buf, sizeof(buf), "%Y/%[4Q]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "2009/cc07/14/02/blubber.log");

    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[1];
    path = mg_get_logfile_path(buf, sizeof(buf), "%Y/%[20U]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "2009/http.examplefa0ce5b0/14/02/blubber.log");

    // edge case; should not produce a hash
    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[1];
    path = mg_get_logfile_path(buf, sizeof(buf), "%Y/%[56U]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "2009/http.example.com.Oops.I.did.it.again.yeah.yeah.yeah.errr/14/02/blubber.log");
    path = mg_get_logfile_path(buf, sizeof(buf), "%Y/%[55U]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "2009/http.example.com.Oops.I.did.it.again.yeah.yeah.fa0ce5b0/14/02/blubber.log");

    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[1];
    path = mg_get_logfile_path(buf, sizeof(buf), "%Y/%[20U]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "2009/http.examplefa0ce5b0/14/02/blubber.log");

    // hash from raw; place at end of scrubbed uri component:
    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = uri_input[1];
    path = mg_get_logfile_path(buf, sizeof(buf), "%Y/%[20Q]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "2009/_ohhhhh.you_d2d6cc07/14/02/blubber.log");

    // IPv4 ports:
    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    c.request_info.uri = NULL;
    ASSERT(parse_ipvX_addr_string("10.11.12.13", 80, &c.client.lsa));
    ASSERT(parse_ipvX_addr_string("120.121.122.123", 180, &c.client.rsa));
    path = mg_get_logfile_path(buf, sizeof(buf), "%Y/%[C]/%[P]/%[s]/%[p]/%d/%m/blubber.log", &c, 1234567890);
    ASSERT(path);
    ASSERT_STREQ(path, "2009/120.121.122.123/180/10.11.12.13/80/14/02/blubber.log");



    // test illegal %[ formatters:
    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    path = mg_get_logfile_path(buf, sizeof(buf), "%[?].long-blubber.log", &c, time(NULL));
    ASSERT(path);
    ASSERT_STREQ(path, "![?].long-blubber.log"); // Note: we don't sanitize the template bits that originate in the server itself, and rightly so. Hence the '?' in here.

    memset(&c, 0, sizeof(c)); c.ctx = ctx;
    path = mg_get_logfile_path(buf, sizeof(buf), "%[bugger].%[-12345678901234567890boo].%[20~].long-blubber.log", &c, time(NULL));
    ASSERT(path);
    ASSERT_STREQ(path, "![bugger].![boo].![~].long-blubber.log");
}

/*
Fail on line 338: strings not matching: inp:"2009/http.example.com.Oops.I.did.it.again.yeah.yeah.yeah.errr/14/02/blubber.log" != ref:"2009/http.example.com.Oops.I.didfa0ce5b0/14/02/blubber.log"
Fail on line 341: strings not matching: inp:"2009/http.example.com.Oops.I.did.it.again.yeah.yeah.fa0ce5b0/14/02/blubber.log" != ref:"2009/http.example.com.Oops.I.didfa0ce5b0/14/02/blubber.log"
*/

static void test_header_processing()
{
    static const char *input = "HTTP/1.0 302 Found\r\n"
        "Location: http://www.google.nl/\r\n"
        "Cache-Control: private\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Set-Cookie: PREF=ID=f72f677fe44bc3d1:FF=0:TM=17777451416:LM=17777451416:S=SkqoabbgNkQJ-8ZZ; expires=Thu, 03-Apr-2014 08:23:36 GMT; path=/; domain=.google.com\r\n"
        "Set-Cookie: NID=58=zWkgbt1WtGE2ahsyDK_yNQDUaCaJ-3cWNNT-xMtBQyohMdaAtO9cHXaFZ23No4FfVXK-0jFVAVRUOiTy9KfmvHP1C0crTZjwWPIjORoR-kUqxXkf6MAxTR4hgPd8CzLF; expires=Wed, 03-Oct-2012 08:23:36 GMT; path=/; domain=.google.com; HttpOnly\r\n"
        "P3P: CP=\"This is not a P3P policy! See http://www.google.com/support/accounts/bin/answer.py?hl=en&answer=151657 for more info.\"\r\n"
        "Date: Tue, 03 Apr 2012 08:43:46 GMT\r\n"
        "Server: gws\r\n"
        "Content-Length: 218\r\n"
        "X-XSS-Protection: 1; mode=block\r\n"
        "X-Frame-Options: SAMEORIGIN\r\n"
        "\r\n"
        "<HTML><HEAD><meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\">\r\n"
        "<TITLE>302 Moved</TITLE></HEAD><BODY>\r\n"
        "<H1>302 Moved</H1>\r\n"
        "The document has moved\r\n"
        "<A HREF=\"http://www.google.nl/\">here</A>.\r\n"
        "</BODY></HTML>\r\n";

  char buf[8192];
  struct mg_context ctx_fake = {0};
  struct mg_context *ctx = &ctx_fake;
  struct mg_connection c = {0};
  int rv;
  char *p;
  const char *values[64];

    c.ctx = ctx;

    strcpy(buf, input);

    rv = get_request_len(buf, (int)strlen(buf));
    ASSERT(rv > 0 && rv < (int)strlen(buf));
    ASSERT(strstr(buf + rv, "<HTML><HEAD>") == buf + rv);
    buf[rv] = 0;
    p = buf;
    parse_http_headers(&p, &c.request_info);
    ASSERT(p > buf);
    ASSERT(*p == 0);
    ASSERT(c.request_info.num_headers == 11);

    values[0] = mg_get_header(&c, "Set-Cookie");
    ASSERT(values[0]);

    rv = mg_get_headers(values, 64, &c, "Set-Cookie");
    ASSERT(rv == 2);
    ASSERT(values[0]);
    ASSERT(values[1]);
    ASSERT(!values[2]);

    rv = mg_get_headers(values, 2, &c, "Set-Cookie");
    ASSERT(rv == 2);
    ASSERT(values[0]);
    ASSERT(!values[1]);

    rv = mg_get_headers(values, 1, &c, "Set-Cookie");
    ASSERT(rv == 2);
    ASSERT(!values[0]);

    values[0] = mg_get_header(&c, "p3p");
    ASSERT(values[0]);

    values[0] = mg_get_header(&c, "NID");
    ASSERT(!values[0]);

    values[0] = mg_get_header(&c, "PREF");
    ASSERT(!values[0]);

    values[0] = mg_get_header(&c, "Cache-Control");
    ASSERT(values[0]);

    values[0] = mg_get_header(&c, "X-XSS-Protection");
    ASSERT(values[0]);

    rv = mg_get_headers(values, 64, &c, "Content-Type");
    ASSERT(values[0]);
    ASSERT(rv == 1);

    rv = mg_get_headers(values, 64, &c, "content-type");
    ASSERT(values[0]);
    ASSERT(rv == 1);

    rv = mg_get_headers(values, 64, &c, "CONTENT-TYPE");
    ASSERT(values[0]);
    ASSERT(rv == 1);
}





static void test_client_connect() {
  char buf[512];
  struct mg_context ctx_fake = {0};
  struct mg_context *ctx = &ctx_fake;
  struct mg_connection *g;
  struct mg_request_info *ri;
  int rv;
  const char *cookies[16];
  int cl;

    g = mg_connect_to_host(ctx, "example.com", 80, MG_CONNECT_BASIC);
    ASSERT(g);

    rv = mg_printf(g, "GET / HTTP/1.0\r\n\r\n");
    ASSERT(rv == 18);
    mg_shutdown(g, SHUT_WR);
    rv = mg_read(g, buf, sizeof(buf));
    ASSERT(rv > 0);
    mg_close_connection(g);
    //free(g);


    g = mg_connect_to_host(ctx, "google.com", 80, MG_CONNECT_USE_SSL);
    ASSERT(!g);
    g = mg_connect_to_host(ctx, "google.com", 80, MG_CONNECT_BASIC);
    ASSERT(g);

    rv = mg_printf(g, "GET / HTTP/1.0\r\n\r\n");
    ASSERT(rv == 18);
    mg_shutdown(g, SHUT_WR);
    rv = mg_read(g, buf, sizeof(buf));
    ASSERT(rv > 0);
    mg_close_connection(g);
    //free(g);


    // now with HTTP header support:
    ASSERT_STREQ(get_option(ctx, MAX_REQUEST_SIZE), "");
    g = mg_connect_to_host(ctx, "www.google.com", 80, MG_CONNECT_BASIC | MG_CONNECT_HTTP_IO);
    ASSERT(!g);

    // all options are empty
    ASSERT_STREQ(get_option(ctx, MAX_REQUEST_SIZE), "");
    // so we should set them up, just like one would've got when calling mg_start():
    ctx->config[MAX_REQUEST_SIZE] = "256";

    g = mg_connect_to_host(ctx, "www.google.com", 80, MG_CONNECT_BASIC | MG_CONNECT_HTTP_IO);
    ASSERT(g);

    ASSERT(0 == mg_add_tx_header(g, 0, "Host", "www.google.com"));
    ASSERT(0 == mg_add_tx_header(g, 0, "Connection", "close"));
    // set up the request the rude way: directly patch the request_info struct. Nasty!
    //
    // Setting us up cf. https://developers.google.com/custom-search/docs/xml_results?hl=en#WebSearch_Request_Format
    ri = mg_get_request_info(g);
    ri->http_version = "1.1";
    ri->query_string = "q=mongoose&num=5&client=google-csbe&ie=utf8&oe=utf8&cx=00255077836266642015:u-scht7a-8i";
    ri->request_method = "GET";
    ri->uri = "/search";
    
    rv = mg_write_http_request_head(g, NULL, NULL);
    ASSERT(rv == 153);
    // signal request phase done:
    mg_shutdown(g, SHUT_WR);
    // fetch response, blocking I/O:
    //
    // but since this is a HTTP I/O savvy connection, we should first read the headers and parse them:
    rv = mg_read_http_response(g);
    // google will spit back more tan 256-1 header bytes in its response, so we'll get a buffer overrun:
    ASSERT(rv == 413);
    ASSERT(g->data_len == 256);
    mg_close_connection(g);



    // retry with a suitably large buffer:
    ctx->config[MAX_REQUEST_SIZE] = "2048";

    g = mg_connect_to_host(ctx, "www.google.com", 80, MG_CONNECT_BASIC | MG_CONNECT_HTTP_IO);
    ASSERT(g);

    ASSERT(0 == mg_add_tx_header(g, 0, "Host", "www.google.com"));
    ASSERT(0 == mg_add_tx_header(g, 0, "Connection", "close"));
    // set up the request the rude way: directly patch the request_info struct. Nasty!
    //
    // Setting us up cf. https://developers.google.com/custom-search/docs/xml_results?hl=en#WebSearch_Request_Format
    ri = mg_get_request_info(g);
    ri->http_version = "1.1";
    ri->query_string = "q=mongoose&num=5&client=google-csbe&ie=utf8&oe=utf8&cx=00255077836266642015:u-scht7a-8i";
    ri->request_method = "GET";
    ri->uri = "/search";
    
    rv = mg_write_http_request_head(g, NULL, NULL);
    ASSERT(rv == 153);
    // signal request phase done:
    mg_shutdown(g, SHUT_WR);
    // fetch response, blocking I/O:
    //
    // but since this is a HTTP I/O savvy connection, we should first read the headers and parse them:
    rv = mg_read_http_response(g);
    // google will spit back more tan 256-1 header bytes in its response, so we'll get a buffer overrun:
    ASSERT(rv == 0);
    ASSERT_STREQ(mg_get_header(g, "Connection"), "close");
    ASSERT(mg_get_headers(cookies, ARRAY_SIZE(cookies), g, "Set-Cookie") > 0);
    cl = atoi(mg_get_header(g, "Content-Length"));
    ASSERT(cl > 0);

    // and now fetch the content:
    rv = mg_read(g, buf, sizeof(buf));
    ASSERT(rv > 0);
    ASSERT(rv == cl);
    ASSERT(mg_get_request_info(g));
    ASSERT(mg_get_request_info(g)->status_code == 302 /* Moved */ );

    mg_close_connection(g);
    //free(g);



    // now with _full_ HTTP header support:
    g = mg_connect_to_host(ctx, "www.google.com", 80, MG_CONNECT_BASIC | MG_CONNECT_HTTP_IO);
    ASSERT(g);

    mg_add_tx_header(g, 0, "Host", "www.google.com");
    mg_add_tx_header(g, 0, "Connection", "close");
    // Setting us up cf. https://developers.google.com/custom-search/docs/xml_results?hl=en#WebSearch_Request_Format
    rv = mg_write_http_request_head(g, "GET", "%s?%s", "/search", "q=mongoose&num=5&client=google-csbe&ie=utf8&oe=utf8&cx=00255077836266642015:u-scht7a-8i");
    ASSERT(rv == 153);
    // signal request phase done:
    mg_shutdown(g, SHUT_WR);
    // fetch response, blocking I/O:
    //
    // but since this is a HTTP I/O savvy connection, we should first read the headers and parse them:
    rv = mg_read_http_response(g);
    ASSERT(rv == 0);
    cl = atoi(mg_get_header(g, "Content-Length"));
    ASSERT(cl > 0);
    ASSERT(mg_get_request_info(g));
    ASSERT(mg_get_request_info(g)->status_code == 302 /* Moved */ );

    // and now fetch the content:
    rv = mg_read(g, buf, sizeof(buf));
    ASSERT(rv > 0);
    ASSERT(rv == cl);

    mg_close_connection(g);
    //free(g);



    // check the new google search page at /cse
    g = mg_connect_to_host(ctx, "www.google.com", 80, MG_CONNECT_BASIC | MG_CONNECT_HTTP_IO);
    ASSERT(g);

    // http://www.google.com/cse?q=mongoose&num=5&client=google-csbe&ie=utf8&oe=utf8&cx=00255077836266642015:u-scht7a-8i
    mg_add_tx_header(g, 0, "Host", "www.google.com");
    mg_add_tx_header(g, 0, "Connection", "close");
    rv = mg_write_http_request_head(g, "GET", "%s%s%s", "/cse", "?q=mongoose", "&num=5&client=google-csbe&ie=utf8&oe=utf8&cx=00255077836266642015:u-scht7a-8i");
    ASSERT(rv == 150);
    // signal request phase done:
    mg_shutdown(g, SHUT_WR);
    // fetch response, blocking I/O:
    //
    // but since this is a HTTP I/O savvy connection, we should first read the headers and parse them:
    rv = mg_read_http_response(g);
    ASSERT(rv == 0);
    ASSERT(mg_get_request_info(g));
    ASSERT(mg_get_request_info(g)->status_code == 404); // funny thing: google doesn't like this; sends a 404; see next for a 'valid' eqv. request: note the '&amp;' vs '&' down there
    ASSERT_STREQ(mg_get_request_info(g)->http_version, "1.1");

    // and now fetch the content:
    for (;;) {
      int r = mg_read(g, buf, sizeof(buf));

      if (r > 0)
        rv += r;
      else
        break;
    }
    ASSERT(rv > 0);
    //ASSERT(rv == cl);

    mg_close_connection(g);
    //free(g);



    // again: check the new google search page at /cse
    g = mg_connect_to_host(ctx, "www.google.com", 80, MG_CONNECT_BASIC | MG_CONNECT_HTTP_IO);
    ASSERT(g);

    // http://www.google.com/cse?q=mongoose&amp;num=5&amp;client=google-csbe&amp;ie=utf8&amp;oe=utf8&amp;cx=00255077836266642015:u-scht7a-8i
    mg_add_tx_header(g, 0, "Host", "www.google.com");
    mg_add_tx_header(g, 0, "Connection", "close");
    rv = mg_write_http_request_head(g, "GET", "%s%s%s", "/cse", "?q=mongoose", "&amp;num=5&amp;client=google-csbe&amp;ie=utf8&amp;oe=utf8&amp;cx=00255077836266642015:u-scht7a-8i");
    ASSERT(rv == 170);
    // signal request phase done:
    mg_shutdown(g, SHUT_WR);
    // fetch response, blocking I/O:
    //
    // but since this is a HTTP I/O savvy connection, we should first read the headers and parse them:
    rv = mg_read_http_response(g);
    ASSERT(rv == 0);
    ASSERT(NULL == mg_get_header(g, "Content-Length")); // google doesn't send a Content-Length with this one
    ASSERT(mg_get_request_info(g));
    ASSERT(mg_get_request_info(g)->status_code == 200);
    ASSERT_STREQ(mg_get_request_info(g)->http_version, "1.1");

    // and now fetch the content:
    for (;;) {
      int r = mg_read(g, buf, sizeof(buf));

      if (r > 0)
        rv += r;
      else
        break;
    }
    ASSERT(rv > 0);
    //ASSERT(rv == cl);

    mg_close_connection(g);
    //free(g);
}




static void *chunky_server_callback(enum mg_event event, struct mg_connection *conn) {
  struct mg_request_info *request_info = mg_get_request_info(conn);
  struct mg_context *ctx = mg_get_context(conn);
  char content[1024];
  int content_length;

  if (event == MG_NEW_REQUEST &&
      strstr(request_info->uri, "/chunky")) {
	int chunk_size;
	int chunk_count;
	int i, c;

	if (mg_get_var(request_info->query_string, (size_t)-1, "chunk_size", content, sizeof(content), 0) > 0) {
	  chunk_size = atoi(content);
	} else {
	  chunk_size = 0;
	}
	if (mg_get_var(request_info->query_string, (size_t)-1, "count", content, sizeof(content), 0) > 0) {
	  chunk_count = atoi(content);
	} else {
	  chunk_size = 50;
	}

    mg_add_response_header(conn, 0, "Content-Length", "1234"); // fake; should be removed by the next one:
	// mongoose auto-detects TE when you set the proper header and use mg_write_http_response_head()
    mg_add_response_header(conn, 0, "Transfer-Encoding", "chunked");
    mg_add_response_header(conn, 0, "Content-Type", "text/html");
    ASSERT_STREQ(mg_suggest_connection_header(conn), "close");  // mongoose plays it safe as long as it doesn't know the Status Code yet!
	mg_set_response_code(conn, 200);
    ASSERT_STREQ(mg_suggest_connection_header(conn), "keep-alive");
    //mg_add_response_header(conn, 0, "Connection", "%s", mg_suggest_connection_header(conn)); -- not needed any longer

	// leading whitespace will be ignored:
    mg_add_response_header(conn, 0, "X-Mongoose-UnitTester", "%s%s", "   ", "Millenium Hand and Shrimp");

    mg_write_http_response_head(conn, 0, NULL);

	// because we wish to test RX chunked reception, we set the chunk sizes explicitly for every chunk:
	mg_set_tx_next_chunk_size(conn, chunk_size);

	// any header added/changed AFTER the HEAD was written will be appended to the tail chunk
    mg_add_response_header(conn, 0, "X-Mongoose-UnitTester", "Buggerit!");

    // send a test page, in chunks
    mg_printf(conn, 
              "<html><body><h1>Chunky page</h1>\n"
              "<p><a href=\"/chunky\">Click here</a> to get "
              "the chunky page again.</p>\n");

	do {
	  // because we wish to test RX chunked reception, we set the chunk sizes explicitly for every chunk:
	  mg_set_tx_next_chunk_size(conn, chunk_size);
	  // you may call mg_set_tx_next_chunksize() as often as you like; it only takes effect when a new chunk is generated

	  i = (int)mg_get_tx_remaining_chunk_size(conn);
	  c = mg_get_tx_chunk_no(conn);

  	  // any header added/changed AFTER the HEAD was written will be appended to the tail chunk
      mg_add_response_header(conn, 0, "X-Mongoose-Chunky", "Alter-%d-of-%d", i, c);

	  mg_printf(conn, 
				"\n<pre>\n chunk #%d / %d, (size?: %d) remaining: %d \n</pre>\n"
				"<p>And this is some more lorem ipsum bla bla used as filler for the chunks...</p>\n",
				c, chunk_count, chunk_size, i);
	} while (c < chunk_count);

	i = (int)mg_get_tx_remaining_chunk_size(conn);
	c = mg_get_tx_chunk_no(conn);

	mg_printf(conn, 
			  "\n<pre>\n chunk #%d, remaining: %d \n</pre>\n"
			  "<p><b>Now we've reached the end of our chunky page.</b></p>\n"
			  "<blockquote><p><b>Note</b>: When you look at the page source,\n"
			  "            you may see extra whitespace padding at the end\n"
			  "            of the page to fill the last chunk (if the chunk size\n"
			  "            was rather large and there was a lot 'remaining', that is).\n"
			  "</p></blockquote>\n"
			  "<hr><h1>Bye!</h1>\n",
			  c, i);

	// pump out whitespace when the last explicit chunk wasn't entirely filled:
	i = (int)mg_get_tx_remaining_chunk_size(conn);
	mg_printf(conn, "%*s", i, "\n");

    return (void *)1;
  } else if (event == MG_NEW_REQUEST) {
    content_length = mg_snprintf(conn, content, sizeof(content),
                                 "<html><body><p>Hello from mongoose! Remote port: %d."
                                 "<p><a href=\"/chunky\">Click here</a> to receive "
                                 "a Transfer-Encoding=chunked transmitted page from the server.",
                                 request_info->remote_port);

	mg_set_response_code(conn, 200);
    mg_add_response_header(conn, 0, "Content-Length", "%d", content_length);
    mg_add_response_header(conn, 0, "Content-Type", "text/html");
    mg_add_response_header(conn, 0, "Connection", "%s", mg_suggest_connection_header(conn));
    mg_write_http_response_head(conn, 0, NULL);

    mg_write(conn, content, content_length);

    // Mark as processed
    return (void *)1;
  } else {
    return NULL;
  }
}


static int chunky_write_chunk_header(struct mg_connection *conn, int64_t chunk_size, char *chunk_extensions_dstbuf, size_t chunk_extensions_dstbuf_size) {
  int c = mg_get_tx_chunk_no(conn);

  // generate some custom chunk extensions, semi-randomly, to make sure the decoder can cope as well!
  if ((c % 3) == 2) {
	mg_snq0printf(conn, chunk_extensions_dstbuf, chunk_extensions_dstbuf_size, "mongoose-ext=oh-la-la-%d", c);
  }
  return 1; // run default handler; we were just here to add extensions...
}


int test_chunked_transfer(void) {
  struct mg_context *ctx;
  const char *options[] = {"listening_ports", "8080", NULL};
  struct mg_user_class_t ucb = {
    NULL,
    chunky_server_callback
  };
  struct mg_connection *conn;
  char buf[4096];
  int rv;
  int prospect_chunk_size;

  ucb.write_chunk_header = chunky_write_chunk_header;
    ctx = mg_start(&ucb, options);
    if (!ctx)
      return -1;

    printf("Restartable server started on ports %s.\n",
           mg_get_option(ctx, "listening_ports"));

	// open client connection to server and GET and POST chunked content

    conn = mg_connect_to_host(ctx, "localhost", 8080, MG_CONNECT_BASIC | MG_CONNECT_HTTP_IO);
    ASSERT(conn);

	for (prospect_chunk_size = 16; prospect_chunk_size < 4096; prospect_chunk_size *= 2)
	{
		mg_add_tx_header(conn, 0, "Host", "localhost");
		mg_add_tx_header(conn, 0, "Connection", "keep-alive");
		rv = mg_write_http_request_head(conn, "GET", "/chunky?count=%d&chunk_size=%d", 10, prospect_chunk_size);
		ASSERT(rv >= 89);

		// this one is optional here as we didn't send any data:
		mg_flush(conn);
		// signal request phase done:
		//mg_shutdown(g, SHUT_WR);

		// fetch response, blocking I/O:
		//
		// but since this is a HTTP I/O savvy connection, we should first read the headers and parse them:
		rv = mg_read_http_response(conn);
		ASSERT(rv == 0);
		ASSERT(NULL == mg_get_header(conn, "Content-Length")); // reply should NOT contain a Content-Length header!
		ASSERT(mg_get_request_info(conn));
		ASSERT(mg_get_request_info(conn)->status_code == 200);
		ASSERT_STREQ(mg_get_request_info(conn)->http_version, "1.1");
		ASSERT_STREQ(mg_get_header(conn, "Content-Type"), "text/html");
		// leading whitespace will be ignored:
		ASSERT_STREQ(mg_get_header(conn, "X-Mongoose-UnitTester"), "Millenium Hand and Shrimp");
		ASSERT_STREQ(mg_get_header(conn, "Connection"), "keep-alive");

		// and now fetch the content:
		for (;;) {
		  int r = mg_read(conn, buf, sizeof(buf));

		  if (r > 0)
			rv += r;
		  else
			break;
		}
		ASSERT(rv > 0);
		//ASSERT(rv == cl);


		// as we've got a kept-alive connection, we can send another request!
		ASSERT(0 == mg_cleanup_after_request(conn));
	}


	// now do the same for POST requests: send chunked, receive another chunked stream:
	for (prospect_chunk_size = 16; prospect_chunk_size < 4096; prospect_chunk_size *= 2)
	{
		int i, c, chunk_size;
		int rx_state;
		int rcv_amount;

		mg_add_tx_header(conn, 0, "Host", "localhost");
		mg_add_tx_header(conn, 0, "Connection", "keep-alive");
		mg_add_response_header(conn, 0, "Content-Type", "text/plain");
		mg_add_response_header(conn, 0, "Transfer-Encoding", "%s", "chunked"); // '%s'? Just foolin' with ya. 'chunked' mode must be detected AFTER printf-formatting has been applied to value.
		rv = mg_write_http_request_head(conn, "POST", "/chunky?count=%d&chun_size=%d", 10, 128);
		ASSERT(rv == 170);

		//----------------------------------------------------------------------------------------
		// WARNING:
		// We have the test server deliver a 'echo'-alike service, which starts responding
		// before the POST data is transmitted in its entirety.
		// We MUST interleave writing and reading from the connection as we'll otherwise run into
		// TCP buffer flooding issues (see also issue349 work) as the other side of the pipe needs
		// to read data from the pipe before it fills up, or you get yourself a case of deadlock
		// across a TCP connection.
		// 
		// It is worrysome that the client needs to know how the server behaves, transmission-wise,
		// as mg_read_http_response() is a blocking operation. Of course we have 100% knowledge of 
		// our test server here, but we should think this through in light of the Internet as our
		// scope/context, and then we'd quickly realize that we require a NON-BLOCKING means to
		// detect when the server actually started transmitting the response (not just the content, 
		// but the entire response, response line and headers included). 
		//
		// This is where the new mg_is_read_data_available() API comes in. It will check for 
		// any incoming data, non-blocking and at minimal cost (one select() call) and should
		// always be used in your code before invoking mg_read() and friends in a client-side
		// connection setting.
		//----------------------------------------------------------------------------------------

		rx_state = 0;
		rcv_amount = 0;
		// now send our data, CHUNKED. We're using 'auto chunking' here: each printf() will be a separate chunk.
		for (chunk_size = 1; chunk_size <= 2048; chunk_size *= 2)
		{
			// we set the chunk sizes explicitly for every chunk:
			mg_set_tx_next_chunk_size(conn, chunk_size);
			// you may call mg_set_tx_next_chunksize() as often as you like; it only takes effect when a new chunk is generated

			i = (int)mg_get_tx_remaining_chunk_size(conn);
			c = mg_get_tx_chunk_no(conn);

  			// any header added/changed AFTER the HEAD was written will be appended to the tail chunk
			mg_add_response_header(conn, 0, "X-Mongoose-Chunky-CLIENT", "Alter-%d-of-%d", i, c);

			mg_printf(conn, 
					"We're looking at chunk #%d here, (size?: %d) remaining: %d \n\n",
					c, chunk_size, i);
			// for small chunk sizes, we'll have fallen back to 'auto chunking' around now:
			i = (int)mg_get_tx_remaining_chunk_size(conn);
			c = mg_get_tx_chunk_no(conn);
			mg_printf(conn, 
					"chunk #%5d \n"
					"padding: [*s] \n",
				c, chunk_size, i, - MG_MAX(1, chunk_size - 30), "xxx");

			if (mg_is_read_data_available(conn))
			{
				// fetch response, blocking I/O:
				//
				// but since this is a HTTP I/O savvy connection, we should first read the headers and parse them:
				switch (rx_state)
				{
				case 0:
					rv = mg_read_http_response(conn);
					ASSERT(rv == 0);
					ASSERT(NULL == mg_get_header(conn, "Content-Length")); // reply should NOT contain a Content-Length header!
					ASSERT(mg_get_request_info(conn));
					ASSERT(mg_get_request_info(conn)->status_code == 200);
					ASSERT_STREQ(mg_get_request_info(conn)->http_version, "1.1");
					ASSERT_STREQ(mg_get_header(conn, "Content-Type"), "text/html");
					// leading whitespace will be ignored:
					ASSERT_STREQ(mg_get_header(conn, "X-Mongoose-UnitTester"), "Millenium Hand and Shrimp");
					ASSERT_STREQ(mg_get_header(conn, "Connection"), "keep-alive");

					rx_state++;
					break;

				case 1:
					rv = mg_read(conn, buf, sizeof(buf));

					ASSERT(rv >= 0);
					rcv_amount += rv;
					break;

				default:
					ASSERT(!"should never get here");
					break;
				}
			}
		}
		
		// make sure we mark the chunked transmission as finished!
		mg_flush(conn);
		// signal request phase done:
		//mg_shutdown(g, SHUT_WR);


		// and now fetch the remaining content:
		for (;;) {
		  rv = mg_read(conn, buf, sizeof(buf));

		  ASSERT(rv >= 0);
		  rcv_amount += rv;
		}
		ASSERT(rv == 0);
		ASSERT(rcv_amount > 0);

		// as we've got a kept-alive connection, we can send another request!
		ASSERT(0 == mg_cleanup_after_request(conn));
	}


    mg_close_connection(conn);
    //free(g);


	// now stop the server: done testing
    mg_stop(ctx);
    printf("Server stopped.\n");

  mg_sleep(1000);
  printf("Server terminating now.\n");
  return 0;
}


int main(void) {
#if defined(_WIN32) && !defined(__SYMBIAN32__)
  InitializeCriticalSection(&global_log_file_lock.lock);
  global_log_file_lock.active = 1;
#if _WIN32_WINNT >= _WIN32_WINNT_NT4_SP3
  InitializeCriticalSectionAndSpinCount(&DisconnectExPtrCS, 1000);
#else
  InitializeCriticalSection(&DisconnectExPtrCS);
#endif
#endif

  test_match_prefix();
  test_remove_double_dots();
  test_IPaddr_parsing();
  test_logpath_fmt();
  test_header_processing();
  test_should_keep_alive();
  test_parse_http_request();

#if defined(_WIN32) && !defined(__SYMBIAN32__)
  {
    WSADATA data;
    WSAStartup(MAKEWORD(2,2), &data);
  }
#endif // _WIN32

  test_client_connect();
  test_chunked_transfer();
  return 0;
}
