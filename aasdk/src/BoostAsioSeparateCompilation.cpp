// BOOST_ASIO_SEPARATE_COMPILATION is set for all aasdk modules, so the
// boost::asio .ipp implementations are not inlined into headers. This
// translation unit provides those definitions exactly once for the whole
// program; the linker pulls this object out of the aasdk static archive
// whenever another object references an asio detail symbol.
#include <boost/asio/impl/src.hpp>
