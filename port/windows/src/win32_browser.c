/*
WIN32_BROWSER.C

The game list server's requests (port/linux/src/posix.h's
posix_browser_request, in place of posix_browser.c) with WinHTTP, which
checks an https:// server's certificate against Windows's own certificate
store, as the updater's downloads (win32_update.c).
*/

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

#include "posix.h"

#define BROWSER_USER_AGENT L"halo-ce-universal-browser"
#define TIMEOUT_MILLISECONDS 10000

/* (WinHTTP's TLS 1.3 flag, missing from older SDKs) */
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

static void set_error(char *error, int error_size, const char *what)
{
	DWORD code = GetLastError();

	if (error && error_size > 0)
		snprintf(error, (size_t)error_size, "%s (error %lu)", what, (unsigned long)code);
}

int posix_browser_request(const char *url, const char *body, const char *content_type, char *response,
	int response_size, char *error, int error_size)
{
	wchar_t wide_url[2048], host[256], url_path[2048], headers[256];
	URL_COMPONENTS components;
	HINTERNET session = NULL, connection = NULL, request = NULL;
	DWORD status = 0, status_size = sizeof(status);
	DWORD protocols;
	DWORD body_size = body ? (DWORD)strlen(body) : 0;
	int used = 0;
	int result = 0;

	if (response && response_size > 0)
		response[0] = 0;
	if (error && error_size > 0)
		error[0] = 0;
	if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wide_url, 2048))
	{
		snprintf(error, (size_t)error_size, "a bad address");
		return 0;
	}
	memset(&components, 0, sizeof(components));
	components.dwStructSize = sizeof(components);
	components.lpszHostName = host;
	components.dwHostNameLength = 256;
	components.lpszUrlPath = url_path;
	components.dwUrlPathLength = 2048;
	if (!WinHttpCrackUrl(wide_url, 0, 0, &components) ||
		(components.nScheme != INTERNET_SCHEME_HTTPS && components.nScheme != INTERNET_SCHEME_HTTP))
	{
		snprintf(error, (size_t)error_size, "not an http:// or https:// address: %s", url);
		return 0;
	}
	session = WinHttpOpen(BROWSER_USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
	{
		set_error(error, error_size, "could not start WinHTTP");
		goto done;
	}
	/* (TLS 1.2 and 1.3; Windows versions without 1.3 take 1.2) */
	protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
	if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
	{
		protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
		WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
	}
	WinHttpSetTimeouts(session, TIMEOUT_MILLISECONDS, TIMEOUT_MILLISECONDS, TIMEOUT_MILLISECONDS,
		TIMEOUT_MILLISECONDS);
	connection = WinHttpConnect(session, host, components.nPort, 0);
	if (connection)
	{
		request = WinHttpOpenRequest(connection, body ? L"POST" : L"GET", url_path, NULL, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
	}
	if (!request)
	{
		set_error(error, error_size, "could not reach the server");
		goto done;
	}
	/* (a POST's body: form fields, unless it says otherwise) */
	_snwprintf(headers, sizeof(headers) / sizeof(headers[0]), L"Content-Type: %hs\r\n",
		content_type ? content_type : "application/x-www-form-urlencoded");
	headers[sizeof(headers) / sizeof(headers[0]) - 1] = 0;
	if (!WinHttpSendRequest(request, body ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, body ? (DWORD)-1L : 0,
			body ? (void *)body : WINHTTP_NO_REQUEST_DATA, body_size, body_size, 0) ||
		!WinHttpReceiveResponse(request, NULL))
	{
		set_error(error, error_size, "could not reach the server");
		goto done;
	}
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
	{
		set_error(error, error_size, "not an HTTP response");
		goto done;
	}
	/* the body, cut to the response's size */
	for (;;)
	{
		char buffer[4096];
		DWORD count = 0;
		int kept;

		if (!WinHttpReadData(request, buffer, sizeof(buffer), &count) || !count)
			break;
		kept = response && used < response_size - 1 ? (int)count : 0;
		if (kept > response_size - 1 - used)
			kept = response_size - 1 - used;
		if (kept > 0)
		{
			memcpy(response + used, buffer, (size_t)kept);
			used += kept;
		}
	}
	if (response && response_size > 0)
		response[used] = 0;
	result = (int)status;

done:
	if (request)
		WinHttpCloseHandle(request);
	if (connection)
		WinHttpCloseHandle(connection);
	if (session)
		WinHttpCloseHandle(session);
	return result;
}
