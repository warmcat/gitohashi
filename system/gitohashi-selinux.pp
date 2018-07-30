module gitohashi-selinux 1.0;

require {
	type reserved_port_t;
	type unconfined_t;
	type httpd_t;
	class tcp_socket name_bind;
	class unix_stream_socket connectto;
}

#============= httpd_t ==============
allow httpd_t reserved_port_t:tcp_socket name_bind;
allow httpd_t unconfined_t:unix_stream_socket connectto;

