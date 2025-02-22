#ifndef art_Framework_Core_PtrRemapper_h
#define art_Framework_Core_PtrRemapper_h
////////////////////////////////////////////////////////////////////////
// PtrRemapper
//
// Class to aid in remapping Ptrs and ProductPtrs in various settings
// from items in one branch to (presumably the equivalent) items in
// another branch of the same type.
//
// This class is primarily for use in product mixing and will be
// properly initialized by the time it is provided as an argument to
// the user-supplied mixing function.
//
// PtrRemapper is a function object, so all of its work is done with
// the apply operator -- operator(). This means that if your mixing
// function has an argument (e.g.)
//
//   art::PtrRemapper const& remap
//
// then the usage is:
//
//   remap(...)
//
// There are several signatures to operator() and because they are all
// templates, they can look fairly impenetrable. It is recommended
// therefore to use this header documentation to decide what signature
// is most appropriate for your use rather than looking below at the
// prototypes or the implementation: there really are "No
// User-serviceable Parts."
//
// Notes common to several signatures.
//
// * With the exception of the (rarely required) signature 10 (see its
// specific documentation), all template arguments are deducible from
// the function arguments and therefore it is not necessary to specify
// them in <>.
//
// * Commonly-used arguments:
//
//     * std::vector<COLLECTION<PROD> const*> const& in
//
//     * OutIter& out
//
//       OutIter is a variable of category insert_iterator (usually
//       created by, for instance std::back_inserter(container)) into a
//       container of Ptr (either PtrVector or some other collection of
//       Ptr).
//
//     * offset is a single offset into the container into which the
//     Ptr points. It should be of type
//     convertible-to-container::size_type.
//
//     * offsets is an arbitrary container of such offsets as described
//     in the documentation in
//     art/Persistency/Common/CollectionUtilities.h.
//
// ------------------
// art::Ptr remapping
// ------------------
//
// Available signatures to operator() and example usage:
//
//  1. Remap a single Ptr.
//
//       Ptr<A> newPtr{remap(oldPtr, offset)};
//
//  2. Remap a single PtrVector.
//
//       PtrVector<A> newPV{remap(oldPV, offset)};
//
//  3. Remap a compatible collection (including PtrVector) of Ptr
// providing begin, end iterators. (This will also remap a compatible
// collection of PtrVector, but not of PtrVector const* -- for the
// latter, see 4-10.)
//
//       PtrVector<A> newPV;
//       remap(oldPV.begin(),
//             oldPV.end(),
//             std::back_inserter(newPV),
//             offset);
//
//  4. Remap and flatten a set of products which are containers of
// Ptrs (which includes PtrVector).
//
//       remap(in, out, offsets)
//
//     where offsets is likely calculated by the appropriate call to
//     art::flattenCollections. See
//     art/Persistency/Common/CollectionUtilities for details.
//
//  5. Remap and flatten a set of containers of Ptrs (including
// PtrVector) which may be obtained from a component of the provided
// product. Provide a free function of the correct signature to return
// a reference to the container of Ptrs given a secondary product,
// e.g.:
//
//       PtrVector<B> const& myfunc(A const* prod) {
//         return prod->myBs();
//       }
//
//       remap(in, out, offsets, &myfunc);
//
//  6. Remap and flatten a set of containers of Ptrs (including
// PtrVector) which may be obtained from a component of the provided
// product. Provide the name of a member function of the provided
// product which is an accessor for the container (taking no
// arguments).
//
//       remap(in, out, offsets, &A::myBs);
//
//  7. Remap and flatten a set of containers of Ptrs (including
// PtrVector) which may be obtained from a component of the provided
// product. Provide the name of a member datum of the provided product
// which is the container.
//
//       remap(in, out, offsets, &A::myBs_);
//
//  8. Remap and flatten a set of containers of Ptrs (including
// PtrVector) which is a component of the provided product using the
// provided accessor member function of a class which is not the
// product.
//
//       class Aprocessor {
//       public:
//         B const& myBs(A const*);
//       };
//
//       Aprocessor myAp;
//
//       remap(in, out, offsets, Aprocessor::myBs, myAp);
//
//     Note: if the compiler complains about an unresolved overload set
//     for this signature, try an explicit:
//
//       const_cast<Aprocessor&>(myAp);
//
//  9. Remap and flatten a set of containers of Ptrs (including
// PtrVector) which is a component of the provided product using the
// provided const accessor member function of a class which is not the
// product.
//
//       class Aprocessor {
//       public:
//         B const& myBs(A const*) const;
//       };
//
//       Aprocessor myAp;
//
//       remap(in, out, offsets, Aprocessor::myBs, myAp);
//
//     Note: if the compiler complains about an unresolved overload set
//     for this signature, try an explicit:
//
//       const_cast<Aprocessor const&>(myAp);
//
// 10. More general version of 5-9. that takes a final argument which is
// of arbitrary type provided it or its operator() has the correct
// signature. The drawback is that one of the template arguments (CONT,
// specifying the type of the collection of Ptrs you wish to remap) is
// not deducible, meaning that instead of:
//
//       remap(...);
//
//     one must type (e.g.):
//
//       remap.operator()<std::vector<art::Ptr<B> > >(...)
//
//     Therefore, 4-9. are the recommended signatures for
//     straightforward client code -- this one is provided for maximum
//     flexibility.
//
// -------------------------
// art::ProductPtr remapping
// -------------------------
//
// ProductPtrs are simpler objects that require no offset--only the
// internal product ID must be adjusted.  There is therefore a limited
// set of remapping facilities for them.
//
// 11. Remap a single art::ProductPtr is achieved via:
//
//     ProductPtr<A> newProductPtr{remap(oldProductPtr)};
//
// 12. Remap a collection of art::ProductPtrs to a single
// art::ProductPtr.  The requirement is that all art::ProductPtrs in
// the collection must refer to the same product.
//
//     ProductPtr<A> newProductPtr{remap(in)};
//
// 13. Remap a collection of ProductPtrs which may be obtained from a
// component of the provided product. Provide the name of a member
// datum of the provided product which is the container.
//
//     ProductPtr<A> newProductPtr{remap(in, &A::myProductPtr_)};
//
// 14. Remap a collection of ProductPtrs which may be obtained from a
// component of the provided product using the provided accessor
// member function of the class (taking no arguments).
//
//     ProductPtr<A> newProductPtr{remap(in, &A::myProductPtr)};
//
////////////////////////////////////////////////////////////////////////

#include "art/Framework/Principal/Event.h"
#include "art/Persistency/Common/CollectionUtilities.h"
#include "canvas/Persistency/Common/EDProductGetter.h"
#include "canvas/Persistency/Common/Ptr.h"
#include "canvas/Persistency/Common/PtrVector.h"
#include "canvas/Persistency/Common/RefCore.h"
#include "canvas/Persistency/Provenance/ProductID.h"
#include "cetlib/exempt_ptr.h"

#include <map>

namespace art {
  class PtrRemapper;
  class ProdToProdMapBuilder;

  namespace PtrRemapperDetail {
    // Function template used by 4.
    template <typename PROD>
    PROD const&
    simpleProdReturner(PROD const* prod)
    {
      return *prod;
    }

    // Function object used by 10.
    template <typename CONT, typename PROD, typename CALLBACK>
    class ContReturner {
    public:
      explicit ContReturner(CALLBACK callback) : callback_(callback) {}
      CONT const&
      operator()(PROD const* prod) const
      {
        return callback_(prod);
      }

    private:
      CALLBACK callback_;
    };

    template <typename CONT, typename PROD>
    class ContReturner<CONT, PROD, CONT const& (PROD::*)() const> {
    public:
      using CALLBACK = CONT const& (PROD::*)() const;
      explicit ContReturner(CALLBACK callback) : callback_(callback) {}
      CONT const&
      operator()(PROD const* prod) const
      {
        return (prod->*callback_)();
      }

    private:
      CALLBACK callback_;
    };

    template <typename CONT, typename PROD>
    class ContReturner<CONT, PROD, CONT const PROD::*> {
    public:
      using CALLBACK = CONT const PROD::*;
      explicit ContReturner(CALLBACK callback) : callback_(callback) {}
      CONT const&
      operator()(PROD const* prod) const
      {
        return prod->*callback_;
      }

    private:
      CALLBACK callback_;
    };
  } // namespace PtrRemapperDetail
} // namespace art

class art::PtrRemapper {
public:
  //////////////////////////////////////////////////////////////////////
  // Signatures for operator() -- see documentation at top of header.

  // --------------
  // Ptr remappings
  // --------------

  // 1.
  template <typename PROD, typename SIZE_TYPE>
  Ptr<PROD> operator()(Ptr<PROD> const& oldPtr, SIZE_TYPE offset) const;

  // 2.
  template <typename PROD, typename SIZE_TYPE>
  PtrVector<PROD> operator()(PtrVector<PROD> const& old,
                             SIZE_TYPE offset) const;

  // 3.
  template <typename InIter, typename OutIter, typename SIZE_TYPE>
  void operator()(InIter beg, InIter end, OutIter out, SIZE_TYPE offset) const;

  // 4.
  template <typename OutIter, typename PROD, typename OFFSETS>
  void operator()(std::vector<PROD const*> const& in,
                  OutIter out,
                  OFFSETS const& offsets) const;

  // 5.
  template <typename CONT, typename OutIter, typename PROD, typename OFFSETS>
  void operator()(std::vector<PROD const*> const& in,
                  OutIter out,
                  OFFSETS const& offsets,
                  CONT const& (*extractor)(PROD const*)) const;

  // 6.
  template <typename CONT, typename OutIter, typename PROD, typename OFFSETS>
  void operator()(std::vector<PROD const*> const& in,
                  OutIter out,
                  OFFSETS const& offsets,
                  CONT const& (PROD::*extractor)() const) const;

  // 7.
  template <typename CONT, typename OutIter, typename PROD, typename OFFSETS>
  void operator()(std::vector<PROD const*> const& in,
                  OutIter out,
                  OFFSETS const& offsets,
                  CONT const PROD::*data) const;

  // 8.
  template <typename PROD,
            typename OutIter,
            typename CONT,
            typename X,
            typename OFFSETS>
  void operator()(std::vector<PROD const*> const& in,
                  OutIter out,
                  OFFSETS const& offsets,
                  CONT const& (X::*extractor)(PROD const*),
                  X& x) const;

  // 9.
  template <typename PROD,
            typename OutIter,
            typename CONT,
            typename X,
            typename OFFSETS>
  void operator()(std::vector<PROD const*> const& in,
                  OutIter out,
                  OFFSETS const& offsets,
                  CONT const& (X::*extractor)(PROD const*)const,
                  X const& x) const;

  // 10.
  template <typename CONT,
            typename CALLBACK,
            typename OutIter,
            typename PROD,
            typename OFFSETS>
  void operator()(std::vector<PROD const*> const& in,
                  OutIter out,
                  OFFSETS const& offsets,
                  CALLBACK extractor) const;

  // ---------------------
  // ProductPtr remappings
  // ---------------------

  // 11.
  template <typename PROD>
  ProductPtr<PROD> operator()(ProductPtr<PROD> const& oldPtr) const;

  // 12.
  template <typename PROD>
  ProductPtr<PROD> operator()(
    std::vector<ProductPtr<PROD> const*> const& in) const;

  // 13.
  template <typename PROD, typename T>
  ProductPtr<T> operator()(std::vector<PROD const*> const& in,
                           ProductPtr<T> const PROD::*data) const;

  // 14.
  template <typename PROD, typename T>
  ProductPtr<T> operator()(std::vector<PROD const*> const& in,
                           ProductPtr<T> const& (PROD::*extractor)()
                             const) const;

private:
  friend class ProdToProdMapBuilder;
  using ProdTransMap_t = std::map<ProductID, ProductID>;

  RefCore newRefCore_(ProductID const incomingProductID) const;

  template <typename PROD>
  static ProductPtr<PROD>
  samePtrAs(ProductPtr<PROD> result, ProductPtr<PROD> old)
  {
    if (old == ProductPtr<PROD>{} or result == old) {
      return result;
    }
    throw Exception(errors::LogicError)
      << "PtrRemapper: cannot create output "
      << TypeID{typeid(ProductPtr<PROD>)}.className()
      << "\ndue to conflicting IDs in input ProductPtrs: " << old.id()
      << " vs. " << result.id() << "\nContact artists@fnal.gov for guidance.\n";
  }

  template <typename T>
  static auto
  unknownProduct_(ProductID const id)
  {
    return Exception(errors::LogicError)
           << "PtrRemapper: cannot create output "
           << TypeID{typeid(T)}.className() << " with ProductID: " << id
           << "\nbecause the product is not known.  Perhaps the output product "
              "was misspecified for product mixing.\n";
  }

  // The following data members are filled by
  // ProdToProdBuilder::populateRemapper, *not* by PtrRemapper.
  ProdTransMap_t prodTransMap_{};
  cet::exempt_ptr<Event const> event_{nullptr};
};

// -------------------
// art::Ptr remappings
// -------------------

// 1.
template <typename PROD, typename SIZE_TYPE>
art::Ptr<PROD>
art::PtrRemapper::operator()(Ptr<PROD> const& oldPtr,
                             SIZE_TYPE const offset) const
{
  if (!oldPtr.id().isValid() || oldPtr.isNull()) {
    return {};
  }

  auto core = newRefCore_(oldPtr.id());
  if (core.productGetter()) {
    return Ptr<PROD>{core.id(), oldPtr.key() + offset, core.productGetter()};
  }

  throw unknownProduct_<Ptr<PROD>>(core.id());
}

// 2.
template <typename PROD, typename SIZE_TYPE>
art::PtrVector<PROD>
art::PtrRemapper::operator()(PtrVector<PROD> const& old,
                             SIZE_TYPE const offset) const
{
  PtrVector<PROD> result;
  result.reserve(old.size());
  this->operator()(old.begin(),
                   old.end(),
                   std::back_inserter(result),
                   offset); // 3.
  return result;
}

// 3.
template <typename InIter, typename OutIter, typename SIZE_TYPE>
void
art::PtrRemapper::operator()(InIter const beg,
                             InIter const end,
                             OutIter out,
                             SIZE_TYPE const offset) const
{
  // Need to assume that all Ptr containers and consistent internally
  // and with each other due to a lack of productGetters.

  // Not using transform here allows instantiation for iterator to
  // collection of Ptr or collection of PtrVector.
  for (auto i = beg; i != end; ++i) {
    // Note: this could be signature 1 OR 2 of operator(). If the user
    // calls this signature (3) with iterators into a collection of
    // PtrVector, then the call order will be 3, 2, 3, 1 due to the
    // templates that will be instantiated i.e. the relationship
    // between signatures 2 and 3 is *not* infinitely recursive.
    *out++ = this->operator()(*i, offset); // 1 OR 2.
  }
}

// 4.
template <typename OutIter, typename PROD, typename OFFSETS>
void
art::PtrRemapper::operator()(std::vector<PROD const*> const& in,
                             OutIter out,
                             OFFSETS const& offsets) const
{
  this->operator()(in,
                   out,
                   offsets,
                   PtrRemapperDetail::simpleProdReturner<PROD>); // 5.
}

// 5.
template <typename CONT, typename OutIter, typename PROD, typename OFFSETS>
void
art::PtrRemapper::operator()(std::vector<PROD const*> const& in,
                             OutIter out,
                             OFFSETS const& offsets,
                             CONT const& (*extractor)(PROD const*)) const
{
  this->operator()<CONT, CONT const& (*)(PROD const*)>(in,
                                                       out,
                                                       offsets,
                                                       extractor); // 10.
}

// 6.
template <typename CONT, typename OutIter, typename PROD, typename OFFSETS>
void
art::PtrRemapper::operator()(std::vector<PROD const*> const& in,
                             OutIter out,
                             OFFSETS const& offsets,
                             CONT const& (PROD::*extractor)() const) const
{
  this->operator()<CONT, CONT const& (PROD::*)() const>(in,
                                                        out,
                                                        offsets,
                                                        extractor); // 10.
}

// 7.
template <typename CONT, typename OutIter, typename PROD, typename OFFSETS>
void
art::PtrRemapper::operator()(std::vector<PROD const*> const& in,
                             OutIter out,
                             OFFSETS const& offsets,
                             CONT const PROD::*data) const
{
  this->operator()<CONT, CONT const PROD::*>(in, out, offsets, data); // 10.
}

// 8.
template <typename PROD,
          typename OutIter,
          typename CONT,
          typename X,
          typename OFFSETS>
void
art::PtrRemapper::operator()(std::vector<PROD const*> const& in,
                             OutIter out,
                             OFFSETS const& offsets,
                             CONT const& (X::*)(PROD const*),
                             X& x) const
{
  this->operator()<CONT>(
    in, out, offsets, [&x](auto& elem) { elem.extractor(x); }); // 10.
}

// 9.
template <typename PROD,
          typename OutIter,
          typename CONT,
          typename X,
          typename OFFSETS>
void
art::PtrRemapper::operator()(std::vector<PROD const*> const& in,
                             OutIter out,
                             OFFSETS const& offsets,
                             CONT const& (X::*)(PROD const*)const,
                             X const& x) const
{
  this->operator()<CONT>(
    in, out, offsets, [&x](auto& elem) { elem.extractor(x); }); // 10.
}

// 10.
template <typename CONT,
          typename CALLBACK,
          typename OutIter,
          typename PROD,
          typename OFFSETS>
void
art::PtrRemapper::operator()(std::vector<PROD const*> const& in,
                             OutIter out,
                             OFFSETS const& offsets,
                             CALLBACK extractor) const
{
  if (in.size() != offsets.size()) {
    throw Exception(errors::LogicError)
      << "Collection size of " << in.size()
      << " disagrees with offset container size of " << offsets.size() << ".\n";
  }
  auto i = in.begin();
  auto const e = in.end();
  auto off_iter = offsets.begin();
  PtrRemapperDetail::ContReturner<CONT, PROD, CALLBACK> returner{extractor};
  for (; i != e; ++i, ++off_iter) {
    CONT const& cont{returner.operator()(*i)};
    this->operator()(cont.begin(), cont.end(), out, *off_iter); // 3.
  }
}

// --------------------------
// art::ProductPtr remappings
// --------------------------

// 11.
template <typename PROD>
art::ProductPtr<PROD>
art::PtrRemapper::operator()(ProductPtr<PROD> const& oldPtr) const
{
  if (!oldPtr.id().isValid()) {
    return {};
  }

  auto core = newRefCore_(oldPtr.id());
  if (core.productGetter()) {
    return ProductPtr<PROD>{core.id(), core.productGetter()};
  }

  throw unknownProduct_<ProductPtr<PROD>>(core.id());
}

// 12.
template <typename PROD>
art::ProductPtr<PROD>
art::PtrRemapper::operator()(
  std::vector<ProductPtr<PROD> const*> const& in) const
{
  art::ProductPtr<PROD> result{};
  for (auto const* ptr : in) {
    result = samePtrAs(remap(*ptr), result);
  }
  return result;
}

// 13.
template <typename PROD, typename T>
art::ProductPtr<T>
art::PtrRemapper::operator()(std::vector<PROD const*> const& in,
                             ProductPtr<T> const PROD::*data) const
{
  art::ProductPtr<T> result{};
  for (auto const* ptr : in) {
    result = samePtrAs(this->operator()(ptr->*data), result);
  }
  return result;
}

// 14.
template <typename PROD, typename T>
art::ProductPtr<T>
art::PtrRemapper::operator()(std::vector<PROD const*> const& in,
                             ProductPtr<T> const& (PROD::*extractor)()
                               const) const
{
  art::ProductPtr<T> result{};
  for (auto const* ptr : in) {
    result = samePtrAs(this->operator()((ptr->*extractor)()), result);
  }
  return result;
}

#endif /* art_Framework_Core_PtrRemapper_h */

// Local Variables:
// mode: c++
// End:
