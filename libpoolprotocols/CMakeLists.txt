set(SOURCES
	PoolURI.cpp PoolURI.h
	PoolClient.h
	PoolManager.h PoolManager.cpp
	testing/SimulateClient.h testing/SimulateClient.cpp
	stratum/EthStratumClient.h stratum/EthStratumClient.cpp
	getwork/EthGetworkClient.h getwork/EthGetworkClient.cpp
)

hunter_add_package(OpenSSL)
find_package(OpenSSL REQUIRED)

find_package(jsoncpp CONFIG REQUIRED)

add_library(poolprotocols ${SOURCES})
target_link_libraries(poolprotocols PRIVATE devcore ethminer-buildinfo ethash::ethash Boost::system jsoncpp_lib jsoncpp_object OpenSSL::SSL OpenSSL::Crypto Crypt32)
target_include_directories(poolprotocols PRIVATE ..)
