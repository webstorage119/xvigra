/************************************************************************/
/*                                                                      */
/*     Copyright 2017-2018 by Ullrich Koethe                            */
/*                                                                      */
/*    This file is part of the XVIGRA image analysis library.           */
/*                                                                      */
/*    Permission is hereby granted, free of charge, to any person       */
/*    obtaining a copy of this software and associated documentation    */
/*    files (the "Software"), to deal in the Software without           */
/*    restriction, including without limitation the rights to use,      */
/*    copy, modify, merge, publish, distribute, sublicense, and/or      */
/*    sell copies of the Software, and to permit persons to whom the    */
/*    Software is furnished to do so, subject to the following          */
/*    conditions:                                                       */
/*                                                                      */
/*    The above copyright notice and this permission notice shall be    */
/*    included in all copies or substantial portions of the             */
/*    Software.                                                         */
/*                                                                      */
/*    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND    */
/*    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES   */
/*    OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND          */
/*    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT       */
/*    HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,      */
/*    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING      */
/*    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR     */
/*    OTHER DEALINGS IN THE SOFTWARE.                                   */
/*                                                                      */
/************************************************************************/

#ifndef XVIGRA_DISTANCE_TRANSFORM_HPP
#define XVIGRA_DISTANCE_TRANSFORM_HPP

#include <vector>
#include <xtensor/xeval.hpp>
#include "global.hpp"
#include "concepts.hpp"
#include "math.hpp"
#include "slice.hpp"
#include "functor_base.hpp"

namespace xvigra
{
    namespace detail
    {

        template <class T>
        struct distance_parabola_stack_entry
        {
            double left, center, right;
            T apex_height;

            distance_parabola_stack_entry(T const & p, double l, double c, double r)
            : left(l), center(c), right(r), apex_height(p)
            {}
        };

        /*********************/
        /* distance_parabola */
        /*********************/

        // 'in' contains initial squared distances or infinity (approximated by a large number).
        // 'out' will contain updated squared distances.
        // When 'invert=true', the parabolas open downwards (needed for dilation), otherwise
        // upwards (needed for distance transform and erosion)
        template <class T1, index_t N1, class T2, index_t N2>
        void distance_parabola(view_nd<T1, N1> const & in, view_nd<T2, N2> out, double sigma, bool invert=false)
        {
            // we assume that the data in the input is distance squared and treat it as such
            double w = in.shape()[0];
            if(w <= 0)
                return;

            double sigma2 = sq(sigma);
            if(invert)
            {
                sigma2 = -sigma2;
            }
            double sigma22 = 2.0 * sigma2;

            using influence = distance_parabola_stack_entry<T1>;

            std::vector<influence> _stack;
            _stack.push_back(influence(in(0), 0.0, 0.0, w));

            index_t k = 1;
            for(double current = 1.0; current < w; ++current, ++k)
            {
                double intersection = 0.0;

                while(true)
                {
                    influence & s = _stack.back();
                    double diff = current - s.center;
                    intersection = current + (in(k) - s.apex_height - sigma2*sq(diff)) / (sigma22 * diff);

                    if( intersection < s.left) // previous point has no influence
                    {
                        _stack.pop_back();
                        if(!_stack.empty())
                        {
                            continue;  // try new top of stack without advancing current
                        }
                        else
                        {
                            intersection = 0.0;
                        }
                    }
                    else if(intersection < s.right)
                    {
                        s.right = intersection;
                    }
                    break;
                }

                _stack.push_back(influence(in(k), intersection, current, w));
            }

            // Now we have the stack indicating which points are influenced by (and therefore
            // closest to) which other point. We can go through the stack and calculate the
            // distance squared for each point.
            k = 0;
            auto it = _stack.begin();
            for(double current = 0.0; current < w; ++current, ++k)
            {
                while( current >= it->right)
                {
                    ++it;
                }
                out(k) = sigma2 * sq(current - it->center) + it->apex_height;
            }
        }

        /***************************/
        /* distance_transform_impl */
        /***************************/

        template <class T1, index_t N1, class T2, index_t N2, class SigmaArray>
        void distance_transform_impl(view_nd<T1, N1> const & in, view_nd<T2, N2> out,
                                     SigmaArray const & sigmas, bool invert = false)
        {
            // Sigma is the spread of the parabolas. It determines the structuring element size
            // for ND morphology. When calculating the distance transforms, sigma is usually set to 1,
            // unless one wants to account for anisotropic pixel pitch.
            index_t N = in.dimension();

            slicer nav(in.shape());

            // operate on last dimension first
            nav.set_free_axes(N-1);
            for(; nav.has_more(); ++nav)
            {
                distance_parabola(in.view(*nav), out.view(*nav), sigmas[N-1], invert);
            }

            // operate on further dimensions
            for( index_t d = N-2; d >= 0; --d )
            {
                nav.set_free_axes(d);
                for(; nav.has_more(); ++nav)
                {
                    distance_parabola(out.view(*nav), out.view(*nav), sigmas[d], invert);
                }
            }
        }

     } // namespace detail

    struct distance_transform_squared_functor
    : public functor_base<distance_transform_squared_functor>
    {
        std::string name = "distance_transform_squared";

        template <class T1, index_t N1, class T2, index_t N2, class PitchArray>
        void impl(view_nd<T1, N1> const & in, view_nd<T2, N2> out,
                  bool background, PitchArray const & pixel_pitch) const
        {
            index_t N = in.shape().size();

            double inf = 1.0; // approximation of infinity
            bool pitch_is_real = false;
            for(index_t k=0; k<N; ++k)
            {
                if(int(pixel_pitch[k]) != pixel_pitch[k])
                {
                    pitch_is_real = true;
                }
                inf += sq(pixel_pitch[k]*in.shape()[k]);
            }

            auto lowest  = std::numeric_limits<T2>::lowest(),
                 highest = std::numeric_limits<T2>::max();
            if(std::is_integral<T2>::value && (pitch_is_real || inf > (double)highest))
            {
                // work on a real-valued temporary array
                array_nd<real_promote_type_t<T2>, N2> tmp(out.shape());
                if(background)
                {
                    tmp = where(equal(in, 0), inf, 0.0);
                }
                else
                {
                    tmp = where(not_equal(in, 0), inf, 0.0);
                }

                detail::distance_transform_impl(tmp, tmp, pixel_pitch);

                out = where(tmp > highest, highest, where(tmp < lowest, lowest, round(tmp)));
            }
            else
            {
                // work directly on the destination array
                if(background)
                {
                    out = where(equal(in, 0), inf, 0.0);
                }
                else
                {
                    out = where(not_equal(in, 0), inf, 0.0);
                }

                detail::distance_transform_impl(out, out, pixel_pitch);
            }
        }

        template <class T1, index_t N1, class T2, index_t N2>
        void impl(view_nd<T1, N1> const & in, view_nd<T2, N2> out,
                  bool background = false) const
        {
            std::vector<double> pixel_pitch(in.shape().size(), 1.0);
            impl(in, out, background, pixel_pitch);
        }
    };

    namespace
    {
        distance_transform_squared_functor  distance_transform_squared;

        inline void distance_transform_squared_dummy()
        {
            std::ignore = distance_transform_squared;
        }
    }

    /** \addtogroup DistanceTransform
    */
    //@{

    /******************************/
    /* distance_transform_squared */
    /******************************/

    /** \brief Squared Euclidean distance transform of a multi-dimensional array.

        The algorithm is taken from Donald Bailey: "An Efficient Euclidean Distance Transform",
        Proc. IWCIA'04, Springer LNCS 3322, 2004.

        <b> Declarations:</b>

        pass arbitrary-dimensional array views:
        \code
        namespace vigra {
            // explicitly specify pixel pitch for each coordinate
            template <unsigned int N, class T1, class S1,
                                      class T2, class S2,
                      class Array>
            void
            separableMultiDistSquared(MultiArrayView<N, T1, S1> const & source,
                                      MultiArrayView<N, T2, S2> dest,
                                      bool background,
                                      Array const & pixelPitch);

            // use default pixel pitch = 1.0 for each coordinate
            template <unsigned int N, class T1, class S1,
                                      class T2, class S2>
            void
            separableMultiDistSquared(MultiArrayView<N, T1, S1> const & source,
                                      MultiArrayView<N, T2, S2> dest,
                                      bool background);
        }
        \endcode


        This function performs a squared Euclidean squared distance transform on the given
        multi-dimensional array. Both source and destination
        arrays are represented by iterators, shape objects and accessors.
        The destination array is required to already have the correct size.

        This function expects a mask as its source, where background pixels are
        marked as zero, and non-background pixels as non-zero. If the parameter
        <i>background</i> is true, then the squared distance of all background
        pixels to the nearest object is calculated. Otherwise, the distance of all
        object pixels to the nearest background pixel is calculated.

        Optionally, one can pass an array that specifies the pixel pitch in each direction.
        This is necessary when the data have non-uniform resolution (as is common in confocal
        microscopy, for example).

        This function may work in-place, which means that <tt>siter == diter</tt> is allowed.
        A full-sized internal array is only allocated if working on the destination
        array directly would cause overflow errors (i.e. if
        <tt> NumericTraits<typename DestAccessor::value_type>::max() < N * M*M</tt>, where M is the
        size of the largest dimension of the array.

        <b> Usage:</b>

        <b>\#include</b> \<vigra/multi_distance.hxx\><br/>
        Namespace: vigra

        \code
        Shape3 shape(width, height, depth);
        MultiArray<3, unsigned char> source(shape);
        MultiArray<3, unsigned int> dest(shape);
        ...

        // Calculate Euclidean distance squared for all background pixels
        separableMultiDistSquared(source, dest, true);
        \endcode

        \see vigra::distanceTransform(), vigra::separableMultiDistance()
    */
    // doxygen_overloaded_function(template <...> void separableMultiDistSquared)

    // template <class T1, index_t N1, class T2, index_t N2, class PitchArray>
    // void distance_transform_squared_impl(view_nd<T1, N1> const & in, view_nd<T2, N2> out,
    //                                      bool background, PitchArray const & pixel_pitch)
    // {
    //     index_t N = in.shape().size();

    //     double inf = 1.0; // approximation of infinity
    //     bool pitch_is_real = false;
    //     for(index_t k=0; k<N; ++k)
    //     {
    //         if(int(pixel_pitch[k]) != pixel_pitch[k])
    //         {
    //             pitch_is_real = true;
    //         }
    //         inf += sq(pixel_pitch[k]*in.shape()[k]);
    //     }

    //     auto lowest  = std::numeric_limits<T2>::lowest(),
    //          highest = std::numeric_limits<T2>::max();
    //     if(std::is_integral<T2>::value && (pitch_is_real || inf > (double)highest))
    //     {
    //         // work on a real-valued temporary array
    //         array_nd<real_promote_type_t<T2>, N2> tmp(out.shape());
    //         if(background)
    //         {
    //             tmp = where(equal(in, 0), inf, 0.0);
    //         }
    //         else
    //         {
    //             tmp = where(not_equal(in, 0), inf, 0.0);
    //         }

    //         detail::distance_transform_impl(tmp, tmp, pixel_pitch);

    //         out = where(tmp > highest, highest, where(tmp < lowest, lowest, round(tmp)));
    //     }
    //     else
    //     {
    //         // work directly on the destination array
    //         if(background)
    //         {
    //             out = where(equal(in, 0), inf, 0.0);
    //         }
    //         else
    //         {
    //             out = where(not_equal(in, 0), inf, 0.0);
    //         }

    //         detail::distance_transform_impl(out, out, pixel_pitch);
    //     }
    // }

    // replaced by a functor

    // template <class InArray, class OutArray, class PitchArray>
    // void distance_transform_squared(InArray const & in, OutArray && out,
    //                                 bool background, PitchArray const & pixel_pitch)
    // {
    //     auto && src  = eval_expr(in);
    //     auto && dest = eval_expr(std::forward<OutArray>(out));
    //     distance_transform_squared_impl(make_view(src), make_view(dest), background, pixel_pitch);
    // }

    // template <class InArray, class OutArray>
    // void distance_transform_squared(InArray const & in, OutArray && out,
    //                                 bool background = false)
    // {
    //     std::vector<double> pixel_pitch(in.shape().size(), 1.0);
    //     distance_transform_squared(in, out, background, pixel_pitch);
    // }

    /**********************/
    /* distance_transform */
    /**********************/

    /** \brief Euclidean distance transform of a multi-dimensional array.

        Calls distance_transform_squared() and takes the square root of the result.
    */
    template <class InArray, class OutArray, class PitchArray>
    inline void
    distance_transform(InArray const & in, OutArray && out,
                       bool background, PitchArray const & pixel_pitch)
    {
        distance_transform_squared(in, std::forward<OutArray>(out), background, pixel_pitch);
        out = sqrt(out);
    }

    template <class InArray, class OutArray>
    inline void
    distance_transform(InArray const & in, OutArray && out,
                       bool background = false)
    {
        distance_transform_squared(in, std::forward<OutArray>(out), background);
        out = sqrt(out);
    }

} // namespace xvigra

#if 0

#include <vector>
#include <functional>
#include "array_vector.hxx"
#include "multi_array.hxx"
#include "accessor.hxx"
#include "numerictraits.hxx"
#include "slicer.hxx"
#include "metaprogramming.hxx"
#include "multi_pointoperators.hxx"
#include "functorexpression.hxx"

#include "multi_gridgraph.hxx"     //for boundaryGraph & boundaryMultiDistance
#include "union_find.hxx"        //for boundaryGraph & boundaryMultiDistance

namespace xvigra
{
    // FIXME: port boundary distance transforms

    //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%% BoundaryDistanceTransform %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

    //rewrite labeled data and work with separableMultiDist
    namespace lemon_graph {

    template <class Graph, class T1Map, class T2Map>
    void
    markRegionBoundaries(Graph const & g,
                         T1Map const & labels,
                         T2Map & out)
    {
        typedef typename Graph::NodeIt        graph_scanner;
        typedef typename Graph::OutBackArcIt  neighbor_iterator;

        //find faces
        for (graph_scanner node(g); node != INVALID; ++node)
        {
            typename T1Map::value_type center = labels[*node];

            for (neighbor_iterator arc(g, node); arc != INVALID; ++arc)
            {
                // set adjacent nodes with different labels to 1
                if(center != labels[g.target(*arc)])
                {
                    out[*node] = 1;
                    out[g.target(*arc)] = 1;
                }
            }
        }
    }

    } //-- namspace lemon_graph

    doxygen_overloaded_function(template <...> unsigned int markRegionBoundaries)

    template <unsigned int N, class T1, class S1,
                              class T2, class S2>
    inline void
    markRegionBoundaries(MultiArrayView<N, T1, S1> const & labels,
                         MultiArrayView<N, T2, S2> out,
                         NeighborhoodType neighborhood=DirectNeighborhood)
    {
        vigra_precondition(labels.shape() == out.shape(),
            "markRegionBoundaries(): shape mismatch between input and output.");

        GridGraph<N, undirected_tag> graph(labels.shape(), neighborhood);

        lemon_graph::markRegionBoundaries(graph, labels, out);
    }

    //MultiDistance which works directly on labeled data

    namespace detail
    {

    /********************************************************/
    /*                                                      */
    /*                boundaryDistParabola                  */
    /*                                                      */
    /********************************************************/

    template <class DestIterator, class LabelIterator>
    void
    boundaryDistParabola(DestIterator is, DestIterator iend,
                         LabelIterator ilabels,
                         double dmax,
                         bool array_border_is_active=false)
    {
        // We assume that the data in the input is distance squared and treat it as such
        double w = iend - is;
        if(w <= 0)
            return;

        DestIterator id = is;
        typedef typename LabelIterator::value_type LabelType;
        typedef typename DestIterator::value_type DestType;
        typedef detail::distance_parabola_stack_entry<DestType> Influence;
        typedef std::vector<Influence> Stack;

        double apex_height = array_border_is_active
                                 ? 0.0
                                 : dmax;
        Stack _stack(1, Influence(apex_height, 0.0, -1.0, w));
        LabelType current_label = *ilabels;
        for(double begin = 0.0, current = 0.0; current <= w; ++ilabels, ++is, ++current)
        {
            apex_height = (current < w)
                              ? (current_label == *ilabels)
                                   ? *is
                                   : 0.0
                              : array_border_is_active
                                    ? 0.0
                                    : dmax;
            while(true)
            {
                Influence & s = _stack.back();
                double diff = current - s.center;
                double intersection = current + (apex_height - s.apex_height - sq(diff)) / (2.0 * diff);

                if(intersection < s.left) // previous parabola has no influence
                {
                    _stack.pop_back();
                    if(_stack.empty())
                        intersection = begin; // new parabola is valid for entire present segment
                    else
                        continue;  // try new top of stack without advancing to next pixel
                }
                else if(intersection < s.right)
                {
                    s.right = intersection;
                }
                if(intersection < w)
                    _stack.push_back(Influence(apex_height, intersection, current, w));
                if(current < w && current_label == *ilabels)
                    break; // finished present pixel, advance to next one

                // label changed => finalize the current segment
                typename Stack::iterator it = _stack.begin();
                for(double c = begin; c < current; ++c, ++id)
                {
                    while(c >= it->right)
                        ++it;
                    *id = sq(c - it->center) + it->apex_height;
                }
                if(current == w)
                    break;  // stop when this was the last segment

                // initialize the new segment
                begin = current;
                current_label = *ilabels;
                apex_height = *is;
                Stack(1, Influence(0.0, begin-1.0, begin-1.0, w)).swap(_stack);
                // don't advance to next pixel here, because the present pixel must also
                // be analysed in the context of the new segment
            }
        }
    }

    /********************************************************/
    /*                                                      */
    /*           internalBoundaryMultiArrayDist             */
    /*                                                      */
    /********************************************************/

    template <unsigned int N, class T1, class S1,
                              class T2, class S2>
    void
    internalBoundaryMultiArrayDist(
                          MultiArrayView<N, T1, S1> const & labels,
                          MultiArrayView<N, T2, S2> dest,
                          double dmax, bool array_border_is_active=false)
    {
        typedef typename MultiArrayView<N, T1, S1>::const_traverser LabelIterator;
        typedef typename MultiArrayView<N, T2, S2>::traverser DestIterator;
        typedef MultiArrayslicer<LabelIterator, N> Labelslicer;
        typedef MultiArrayslicer<DestIterator, N> Dslicer;

        dest = dmax;
        for( unsigned d = 0; d < N; ++d )
        {
            Labelslicer lnav( labels.traverser_begin(), labels.shape(), d );
            Dslicer dnav( dest.traverser_begin(), dest.shape(), d );

            for( ; dnav.hasMore(); dnav++, lnav++ )
            {
                boundaryDistParabola(dnav.begin(), dnav.end(),
                                     lnav.begin(),
                                     dmax, array_border_is_active);
            }
        }
    }

    } // namespace detail

        /** \brief Specify which boundary is used for boundaryMultiDistance().

        */
    enum BoundaryDistanceTag {
        OuterBoundary,      ///< Pixels just outside of each region
        InterpixelBoundary, ///< Half-integer points between pixels of different labels
        InnerBoundary       ///< Pixels just inside of each region
    };

    /********************************************************/
    /*                                                      */
    /*             boundaryMultiDistance                    */
    /*                                                      */
    /********************************************************/

    /** \brief Euclidean distance to the implicit boundaries of a multi-dimensional label array.


        <b> Declarations:</b>

        pass arbitrary-dimensional array views:
        \code
        namespace vigra {
            template <unsigned int N, class T1, class S1,
                      class T2, class S2>
            void
            boundaryMultiDistance(MultiArrayView<N, T1, S1> const & labels,
                                  MultiArrayView<N, T2, S2> dest,
                                  bool array_border_is_active=false,
                                  BoundaryDistanceTag boundary=InterpixelBoundary);
        }
        \endcode

        This function computes the distance transform of a labeled image <i>simultaneously</i>
        for all regions. Depending on the requested type of \a boundary, three modes
        are supported:
        <ul>
        <li><tt>OuterBoundary</tt>: In each region, compute the distance to the nearest pixel not
                   belonging to that regions. This is the same as if a normal distance transform
                   where applied to a binary image containing just this region.</li>
        <li><tt>InterpixelBoundary</tt> (default): Like <tt>OuterBoundary</tt>, but shift the distance
                   to the interpixel boundary by subtractiong 1/2. This make the distences consistent
                   accross boundaries.</li>
        <li><tt>InnerBoundary</tt>: In each region, compute the distance to the nearest pixel in the
                   region which is adjacent to the boundary. </li>
        </ul>
        If <tt>array_border_is_active=true</tt>, the
        outer border of the array (i.e. the interpixel boundary between the array
        and the infinite region) is also used. Otherwise (the default), regions
        touching the array border are treated as if they extended to infinity.

        <b> Usage:</b>

        <b>\#include</b> \<vigra/multi_distance.hxx\><br/>
        Namespace: vigra

        \code
        Shape3 shape(width, height, depth);
        MultiArray<3, unsigned char> source(shape);
        MultiArray<3, UInt32> labels(shape);
        MultiArray<3, float> dest(shape);
        ...

        // find regions (interpixel boundaries are implied)
        labelMultiArray(source, labels);

        // Calculate Euclidean distance to interpixel boundary for all pixels
        boundaryMultiDistance(labels, dest);
        \endcode

        \see vigra::distanceTransform(), vigra::separableMultiDistance()
    */
    doxygen_overloaded_function(template <...> void boundaryMultiDistance)

    template <unsigned int N, class T1, class S1,
                              class T2, class S2>
    void
    boundaryMultiDistance(MultiArrayView<N, T1, S1> const & labels,
                          MultiArrayView<N, T2, S2> dest,
                          bool array_border_is_active=false,
                          BoundaryDistanceTag boundary=InterpixelBoundary)
    {
        vigra_precondition(labels.shape() == dest.shape(),
            "boundaryMultiDistance(): shape mismatch between input and output.");

        using namespace vigra::functor;

        if(boundary == InnerBoundary)
        {
            MultiArray<N, unsigned char> boundaries(labels.shape());

            markRegionBoundaries(labels, boundaries, IndirectNeighborhood);
            if(array_border_is_active)
                initMultiArrayBorder(boundaries, 1, 1);
            separableMultiDistance(boundaries, dest, true);
        }
        else
        {
            T2 offset = 0.0;

            if(boundary == InterpixelBoundary)
            {
                vigra_precondition(!NumericTraits<T2>::isIntegral::value,
                    "boundaryMultiDistance(..., InterpixelBoundary): output pixel type must be float or double.");
                offset = T2(0.5);
            }
            double dmax = squaredNorm(labels.shape()) + N;
            if(dmax > double(NumericTraits<T2>::max()))
            {
                // need a temporary array to avoid overflows
                typedef typename NumericTraits<T2>::RealPromote Real;
                MultiArray<N, Real> tmpArray(labels.shape());
                detail::internalBoundaryMultiArrayDist(labels, tmpArray,
                                                          dmax, array_border_is_active);
                transformMultiArray(tmpArray, dest, sqrt(Arg1()) - Param(offset) );
            }
            else
            {
                // can work directly on the destination array
                detail::internalBoundaryMultiArrayDist(labels, dest, dmax, array_border_is_active);
                transformMultiArray(dest, dest, sqrt(Arg1()) - Param(offset) );
            }
        }
    }

    //@}

} //-- namespace vigra

#endif

#endif        //-- XVIGRA_DISTANCE_TRANSFORM_HPP
