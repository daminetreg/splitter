#include "spec_member.hpp"

template <>
int Box<int>::weigh() const { return bump(value); }

// Instantiates scaled(), which calls the specialization above.
template struct Box<int>;
