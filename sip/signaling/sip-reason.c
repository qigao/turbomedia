const char* sip_reason_phrase(int code)
{
	switch (code)
	{
	case 100: return "Trying";
	case 180: return "Ringing";
	case 181: return "Call Is Being Forwarded";
	case 182: return "Queued";
	case 183: return "Session Progress";
	case 200: return "OK";
	case 202: return "Accepted";
	case 300: return "Multiple Choices";
	case 301: return "Moved Permanently";
	case 302: return "Moved Temporarily";
	case 305: return "Use Proxy";
	case 380: return "Alternative Service";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 402: return "Payment Required";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 406: return "Not Acceptable";
	case 407: return "Proxy Authentication Required";
	case 408: return "Request Timeout";
	case 410: return "Gone";
	case 413: return "Request Entity Too Large";
	case 414: return "Request-URI Too Long";
	case 415: return "Unsupported Media Type";
	case 416: return "Unsupported URI Scheme";
	case 420: return "Bad Extension";
	case 421: return "Extension Required";
	case 423: return "Interval Too Brief";
	case 469: return "Bad Info Package";
	case 480: return "Temporarily Unavailable";
	case 481: return "Call/Transaction Does Not Exist";
	case 482: return "Loop Detected";
	case 483: return "Too Many Hops";
	case 484: return "Address Incomplete";
	case 485: return "Ambiguous";
	case 486: return "Busy Here";
	case 487: return "Request Terminated";
	case 488: return "Not Acceptable Here";
	case 491: return "Request Pending";
	case 493: return "Undecipherable";
	case 500: return "Server Internal Error";
	case 501: return "Not Implemented";
	case 502: return "Bad Gateway";
	case 503: return "Service Unavailable";
	case 504: return "Server Time-out";
	case 505: return "Version Not Supported";
	case 513: return "Message Too Large";
	case 600: return "Busy Everywhere";
	case 603: return "Decline";
	case 604: return "Does Not Exist Anywhere";
	case 606: return "Not Acceptable";
	default: return "Unknown";
	}
}
