// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "translate.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

bool is_valid_utf8( const char* text, size_t length ) {
	for( size_t index = 0; index < length; ) {
		const unsigned char first = static_cast<unsigned char>( text[index] );
		if( first <= 0x7F ) {
			++index;
			continue;
		}

		size_t following = 0;
		uint32_t codepoint = 0;
		if( first >= 0xC2 && first <= 0xDF ) {
			following = 1;
			codepoint = first & 0x1F;
		} else if( first >= 0xE0 && first <= 0xEF ) {
			following = 2;
			codepoint = first & 0x0F;
		} else if( first >= 0xF0 && first <= 0xF4 ) {
			following = 3;
			codepoint = first & 0x07;
		} else {
			return false;
		}
		if( index + following >= length ) {
			return false;
		}
		for( size_t offset = 1; offset <= following; ++offset ) {
			const unsigned char next = static_cast<unsigned char>( text[index + offset] );
			if( ( next & 0xC0 ) != 0x80 ) {
				return false;
			}
			codepoint = ( codepoint << 6 ) | ( next & 0x3F );
		}
		if( ( following == 2 && codepoint < 0x800 ) || ( following == 3 && codepoint < 0x10000 ) || codepoint > 0x10FFFF || ( codepoint >= 0xD800 && codepoint <= 0xDFFF ) ) {
			return false;
		}
		index += following + 1;
	}
	return true;
}

std::mutex utf8_stream_mutex;
std::unordered_map<FILE*, bool> utf8_stream_modes;

bool stream_uses_utf8( FILE* stream ) {
	if( stream == nullptr ) {
		return false;
	}
	const long position = ftell( stream );
	if( position < 0 ) {
		return false;
	}
	{
		std::lock_guard<std::mutex> lock( utf8_stream_mutex );
		auto mode = utf8_stream_modes.find( stream );
		if( position != 0 && mode != utf8_stream_modes.end() ) {
			return mode->second;
		}
	}

	if( fseek( stream, 0, SEEK_END ) != 0 ) {
		return false;
	}
	const long file_size = ftell( stream );
	const size_t inspect_size = file_size <= 0 ? 0 : static_cast<size_t>( std::min<long>( file_size, 5 * 1024 * 1024 ) );
	// ===== rAthena 修复 UTF-8 大文件探测边界：开始 =====
	// 修改原因：5 MiB 截止位置可能落在多字节字符中间；最多多读 3 字节即可完成一个 UTF-8 字符。
	const size_t probe_size = file_size > static_cast<long>( inspect_size ) ? inspect_size + 3 : inspect_size;
	std::string content( probe_size, '\0' );
	fseek( stream, 0, SEEK_SET );
	const size_t read_size = probe_size == 0 ? 0 : ::fread( content.data(), 1, probe_size, stream );
	// ===== rAthena 修复 UTF-8 大文件探测边界：结束 =====
	fseek( stream, position, SEEK_SET );
	const bool has_bom = read_size >= 3 && static_cast<unsigned char>( content[0] ) == 0xEF && static_cast<unsigned char>( content[1] ) == 0xBB && static_cast<unsigned char>( content[2] ) == 0xBF;
	const bool result = has_bom || ( read_size > 0 && is_valid_utf8( content.data(), read_size ) );
	std::lock_guard<std::mutex> lock( utf8_stream_mutex );
	utf8_stream_modes[stream] = result;
	return result;
}

#ifdef _WIN32
UINT default_data_codepage() {
	switch( GetUserDefaultUILanguage() ) {
		case 0x0804:
		case 0x1004:
			return 936;
		case 0x0404:
		case 0x0c04:
		case 0x1404:
			return 950;
		default:
			return CP_UTF8;
	}
}

std::atomic<UINT> console_data_codepage{ default_data_codepage() };

UINT encoding_to_codepage( const char* encoding ) {
	std::string name = encoding == nullptr ? "" : encoding;
	for( char& character : name ) {
		character = static_cast<char>( std::tolower( static_cast<unsigned char>( character ) ) );
	}
	if( name == "gbk" || name == "gb2312" || name == "cp936" ) {
		return 936;
	}
	if( name == "big5" || name == "cp950" ) {
		return 950;
	}
	return CP_UTF8;
}

std::string convert_codepage( const char* text, size_t length, UINT source, UINT target ) {
	if( text == nullptr || length == 0 ) {
		return {};
	}
	if( source == target || length > static_cast<size_t>( INT_MAX ) ) {
		return std::string( text, length );
	}
	const int source_length = static_cast<int>( length );
	const int wide_length = MultiByteToWideChar( source, MB_ERR_INVALID_CHARS, text, source_length, nullptr, 0 );
	if( wide_length <= 0 ) {
		return std::string( text, length );
	}
	std::wstring wide( static_cast<size_t>( wide_length ), L'\0' );
	if( MultiByteToWideChar( source, MB_ERR_INVALID_CHARS, text, source_length, wide.data(), wide_length ) <= 0 ) {
		return std::string( text, length );
	}
	const int target_length = WideCharToMultiByte( target, 0, wide.data(), wide_length, nullptr, 0, nullptr, nullptr );
	if( target_length <= 0 ) {
		return std::string( text, length );
	}
	std::string converted( static_cast<size_t>( target_length ), '\0' );
	if( WideCharToMultiByte( target, 0, wide.data(), wide_length, converted.data(), target_length, nullptr, nullptr ) <= 0 ) {
		return std::string( text, length );
	}
	return converted;
}
#endif

} // namespace

void set_console_data_encoding( const char* encoding ) {
#ifdef _WIN32
	console_data_codepage.store( encoding_to_codepage( encoding ) );
#else
	(void)encoding;
#endif
}

std::string normalize_console_output( const char* text, size_t length ) {
#ifdef _WIN32
	return convert_codepage( text, length, console_data_codepage.load(), CP_UTF8 );
#else
	return text == nullptr ? "" : std::string( text, length );
#endif
}

std::string convert_utf8_file_content( const char* text, size_t length, bool preserve_utf8 ) {
	if( text == nullptr || length == 0 ) {
		return {};
	}

	size_t offset = 0;
	if( length >= 3 && static_cast<unsigned char>( text[0] ) == 0xEF && static_cast<unsigned char>( text[1] ) == 0xBB && static_cast<unsigned char>( text[2] ) == 0xBF ) {
		offset = 3;
	}

#ifdef _WIN32
	const int content_length = length - offset > static_cast<size_t>( INT_MAX ) ? -1 : static_cast<int>( length - offset );
	const bool valid_utf8 = content_length >= 0 && is_valid_utf8( text + offset, length - offset );
	if( offset == 0 && !valid_utf8 ) {
		return std::string( text, length );
	}
	if( preserve_utf8 ) {
		return std::string( text + offset, length - offset );
	}
	return convert_codepage( text + offset, length - offset, CP_UTF8, console_data_codepage.load() );
#else
	return std::string( text + offset, length - offset );
#endif
}

char* utf8_fgets( char* buffer, int max_count, FILE* stream ) {
	if( buffer == nullptr || max_count <= 0 || stream == nullptr || !stream_uses_utf8( stream ) ) {
		return ::fgets( buffer, max_count, stream );
	}
	if( ftell( stream ) == 0 ) {
		unsigned char bom[3] = {};
		const size_t read = ::fread( bom, 1, 3, stream );
		if( read != 3 || bom[0] != 0xEF || bom[1] != 0xBB || bom[2] != 0xBF ) {
			fseek( stream, 0, SEEK_SET );
		}
	}
	std::string raw( static_cast<size_t>( max_count ), '\0' );
	if( ::fgets( raw.data(), max_count, stream ) == nullptr ) {
		return nullptr;
	}
	const std::string converted = convert_utf8_file_content( raw.c_str(), strlen( raw.c_str() ) );
	const size_t copy_size = ( std::min )( converted.size(), static_cast<size_t>( max_count - 1 ) );
	memcpy( buffer, converted.data(), copy_size );
	buffer[copy_size] = '\0';
	return buffer;
}
