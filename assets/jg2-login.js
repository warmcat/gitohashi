/*
 * lws-login.js defines window.renderLwsLoginStatus() but never calls it:
 * it expects the host page to kick it once its DOM is available.  The
 * retired client-side renderer used to make the call after building the
 * page; with server-side rendering the #lws-user-info divs arrive in the
 * document itself and this hook is the whole call.
 *
 * If the vhost does not have the login feature, lws-login.js is not
 * served, the function stays undefined and the divs are simply left
 * empty.
 */

(function() {
	function kick() {
		if (typeof window.renderLwsLoginStatus !== 'function' ||
		    !document.getElementById('lws-user-info'))
			return;

		window.renderLwsLoginStatus('lws-user-info');
	}

	if (document.readyState === 'loading')
		document.addEventListener('DOMContentLoaded', kick);
	else
		kick();
})();
