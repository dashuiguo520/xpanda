// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "translate.hpp"

// ===== 终端信息翻译机制移植 开始 =====
// 修改内容/原因：移植 Pandas 的 YAML 终端翻译机制，并移除对 Pandas 专属配置及编码辅助类的依赖。
#include <clocale>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <common/cbasetypes.hpp>
#include <common/core.hpp>
#include <common/database.hpp>
#include <common/showmsg.hpp>
#include <common/timer.hpp>

// ===== UTF-8 配置与脚本读取支持 开始 =====
// 修改内容/原因：本实现必须调用原始 C 文件函数，避免默认读取宏在包装函数内部递归展开。
#undef fgets
// ===== UTF-8 配置与脚本读取支持 结束 =====

#define TRANSLATE_STRINGIFY_INNER( value ) #value
#define TRANSLATE_STRINGIFY( value ) TRANSLATE_STRINGIFY_INNER( value )

namespace {

struct TranslateItem {
	std::string translation;
};

void replace_all( std::string& message, const std::string& from, const std::string& to ) {
	for( size_t position = 0; ( position = message.find( from, position ) ) != std::string::npos; position += to.length() ) {
		message.replace( position, from.length(), to );
	}
}

std::string system_language_suffix() {
#ifdef _WIN32
	switch( GetUserDefaultUILanguage() ) {
		case 0x0804: // Chinese (Simplified, PRC)
		case 0x1004: // Chinese (Simplified, Singapore)
			return "cn";
		case 0x0404: // Chinese (Traditional, Taiwan)
		case 0x0c04: // Chinese (Traditional, Hong Kong)
		case 0x1404: // Chinese (Traditional, Macao)
			return "tw";
		default:
			return {};
	}
#else
	std::setlocale( LC_CTYPE, "" );
	const char* locale_name = std::setlocale( LC_CTYPE, nullptr );
	const std::string locale = locale_name == nullptr ? "" : locale_name;
	if( locale.rfind( "zh_CN", 0 ) == 0 || locale.rfind( "zh_SG", 0 ) == 0 ) {
		return "cn";
	}
	if( locale.rfind( "zh_TW", 0 ) == 0 || locale.rfind( "zh_HK", 0 ) == 0 || locale.rfind( "zh_MO", 0 ) == 0 ) {
		return "tw";
	}
	return {};
#endif
}

class TranslateDatabase final : public TypesafeYamlDatabase<std::string, TranslateItem> {
public:
	TranslateDatabase() : TypesafeYamlDatabase( "CONSOLE_TRANSLATE_DB", 1 ) {
		#define ADD_TRANSLATE_TAG( tag ) tags.emplace_back( "[{" #tag "}]", tag )
		ADD_TRANSLATE_TAG( CL_RESET );
		ADD_TRANSLATE_TAG( CL_CLL );
		ADD_TRANSLATE_TAG( CL_BOLD );
		ADD_TRANSLATE_TAG( CL_NORMAL );
		ADD_TRANSLATE_TAG( CL_WHITE );
		ADD_TRANSLATE_TAG( CL_RED );
		ADD_TRANSLATE_TAG( CL_GREEN );
		ADD_TRANSLATE_TAG( CL_YELLOW );
		ADD_TRANSLATE_TAG( CL_MAGENTA );
		ADD_TRANSLATE_TAG( CL_CYAN );
		ADD_TRANSLATE_TAG( CL_BG_RED );
		ADD_TRANSLATE_TAG( CL_BT_GREEN );
		ADD_TRANSLATE_TAG( CL_BT_CYAN );
		ADD_TRANSLATE_TAG( CL_BT_WHITE );
		ADD_TRANSLATE_TAG( CL_PASS );
		ADD_TRANSLATE_TAG( PRtf );
		ADD_TRANSLATE_TAG( PRIuPTR );
		ADD_TRANSLATE_TAG( PRIdPTR );
		ADD_TRANSLATE_TAG( PRIu64 );
		ADD_TRANSLATE_TAG( PRId64 );
		// PRIi64 是 Pandas 翻译库中的兼容标签；rAthena 使用标准 PRId64 表示相同的十进制格式。
		tags.emplace_back( "[{PRIi64}]", PRId64 );
		ADD_TRANSLATE_TAG( PRIu32 );
		ADD_TRANSLATE_TAG( PRIu16 );
		ADD_TRANSLATE_TAG( PRId16 );
		ADD_TRANSLATE_TAG( PRIXPTR );
		#undef ADD_TRANSLATE_TAG
		tags.emplace_back( "[{EXPAND_AND_QUOTE(PACKETVER)}]", TRANSLATE_STRINGIFY( PACKETVER ) );
	}

	const std::string getDefaultLocation() override {
		const std::string suffix = system_language_suffix();
		return suffix.empty() ? "" : std::string( conf_path ) + "/msg_conf/translation_" + suffix + ".yml";
	}

	uint64 parseBodyNode( const ryml::NodeRef& node ) override {
		std::string original;
		std::string translation;
		if( !this->asString( node, "Original", original ) || !this->asString( node, "Translation", translation ) ) {
			return 0;
		}

		for( const auto& tag : tags ) {
			replace_all( original, tag.first, tag.second );
			replace_all( translation, tag.first, tag.second );
		}

		auto item = std::make_shared<TranslateItem>();
		item->translation = std::move( translation );
		this->put( std::move( original ), std::move( item ) );
		return 1;
	}

private:
	std::vector<std::pair<std::string, std::string>> tags;
};

TranslateDatabase translate_database;
bool translate_initialized = false;

// ===== UTF-8 配置与脚本读取支持 开始 =====
// 修改内容/原因：使用严格 UTF-8 字节规则判定无 BOM 文件，避免把旧 GBK/Big5 文本误转码。
bool is_valid_utf8( const char* text, size_t length ) {
	for( size_t index = 0; index < length; ) {
		const unsigned char first = static_cast<unsigned char>( text[index] );
		if( first <= 0x7F ) {
			++index;
			continue;
		}

		size_t following = 0;
		uint32 codepoint = 0;
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
// ===== UTF-8 配置与脚本读取支持 结束 =====

// ===== 终端动态文本编码桥接 开始 =====
// 修改内容/原因：记录当前服务实际使用的数据库连接代码页，使 UTF-8 模板与动态数据库文本先统一编码再格式化。
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
// ===== 终端动态文本编码桥接 结束 =====

} // namespace

void do_init_translate() {
	// ===== Windows UTF-8 控制台适配 开始 =====
	// 修改内容/原因：翻译库内容为 UTF-8；同步控制台代码页，避免 WriteConsoleA 按 GBK 解释中文而产生乱码。
#ifdef _WIN32
	SetConsoleOutputCP( CP_UTF8 );
	SetConsoleCP( CP_UTF8 );
#endif
	// ===== Windows UTF-8 控制台适配 结束 =====

	if( translate_database.getDefaultLocation().empty() ) {
		return;
	}
	translate_initialized = translate_database.load();
}

void do_final_translate() {
	translate_initialized = false;
	translate_database.clear();
}

std::string translate_console_message( const char* original ) {
	if( original == nullptr ) {
		return {};
	}

	std::string message = original;
	if( translate_initialized ) {
		auto item = translate_database.find( original );
		if( item != nullptr && !item->translation.empty() ) {
			message = item->translation;
		}
	}

	// ===== 终端动态文本编码桥接 开始 =====
	// 修改内容/原因：格式化前把 UTF-8 源码/翻译模板转为动态参数使用的数据库代码页，防止同一缓冲区混合编码。
#ifdef _WIN32
	return convert_codepage( message.data(), message.size(), CP_UTF8, console_data_codepage.load() );
#else
	return message;
#endif
	// ===== 终端动态文本编码桥接 结束 =====
}
// ===== 终端信息翻译机制移植 结束 =====

// ===== 终端动态文本编码桥接 开始 =====
// 修改内容/原因：由成功建立的数据库连接更新动态文本代码页，并在输出前将整行统一转换为 UTF-8。
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
// ===== 终端动态文本编码桥接 结束 =====

// ===== UTF-8 配置与脚本读取支持 开始 =====
// 修改内容/原因：识别 UTF-8 BOM 或严格有效的 UTF-8 内容；旧 GBK/Big5 文件原样保留，UTF-8 文件转换为当前服务内部代码页。
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
	// Linux 服务通常以 UTF-8 作为内部及终端编码；此处至少统一移除可选 BOM，旧编码内容保持字节不变。
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
	const size_t copy_size = std::min( converted.size(), static_cast<size_t>( max_count - 1 ) );
	memcpy( buffer, converted.data(), copy_size );
	buffer[copy_size] = '\0';
	return buffer;
}
// ===== UTF-8 配置与脚本读取支持 结束 =====
