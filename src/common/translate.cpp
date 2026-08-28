// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "translate.hpp"

// ===== 终端信息翻译机制移植 开始 =====
// 修改内容/原因：移植 Pandas 的 YAML 终端翻译机制，并移除对 Pandas 专属配置及编码辅助类的依赖。
#include <clocale>
#include <atomic>
#include <cctype>
#include <climits>
#include <memory>
#include <string>
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

// ===== 终端动态文本编码桥接 开始 =====
// 修改内容/原因：记录当前服务实际使用的数据库连接代码页，使 UTF-8 模板与动态数据库文本先统一编码再格式化。
#ifdef _WIN32
std::atomic<UINT> console_data_codepage{ CP_UTF8 };

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
