# Copyright 2021 Memgraph Ltd.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
# License, and you may not use this file except in compliance with the Business Source License.
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0, included in the file
# licenses/APL.txt.

"""
This module provides the API for usage in custom openCypher procedures.
"""

# C API using `mgp_memory` is not exposed in Python, instead the usage of such
# API is hidden behind Python API. Any function requiring an instance of
# `mgp_memory` should go through a `ProcCtx` instance.
#
# `mgp_value` does not exist as such in Python, instead all `mgp_value`
# instances are marshalled to an appropriate Python object. This implies that
# `mgp_list` and `mgp_map` are mapped to `list` and `dict` respectively.
#
# Only the public API is stubbed out here. Any private details are left for the
# actual implementation. Functions have type annotations as supported by Python
# 3.5, but variable type annotations are only available with Python 3.6+

from collections import namedtuple
from functools import wraps
import inspect
import sys
import typing
import datetime


import _mgp


class InvalidContextError(Exception):
    """
    Signals using a graph element instance outside of the registered procedure.
    """
    pass


class UnknownError(_mgp.UnknownError):
    """
    Signals unspecified failure.
    """
    pass


class UnableToAllocateError(_mgp.UnableToAllocateError):
    """
    Signals failed memory allocation.
    """
    pass


class InsufficientBufferError(_mgp.InsufficientBufferError):
    """
    Signals that some buffer is not big enough.
    """
    pass


class OutOfRangeError(_mgp.OutOfRangeError):
    """
    Signals that an index-like parameter has a value that is outside its
    possible values.
    """
    pass


class LogicErrorError(_mgp.LogicErrorError):
    """
    Signals faulty logic within the program such as violating logical
    preconditions or class invariants and may be preventable.
    """
    pass


class DeletedObjectError(_mgp.DeletedObjectError):
    """
    Signals accessing an already deleted object.
    """
    pass


class InvalidArgumentError(_mgp.InvalidArgumentError):
    """
    Signals that some of the arguments have invalid values.
    """
    pass


class KeyAlreadyExistsError(_mgp.KeyAlreadyExistsError):
    """
    Signals that a key already exists in a container-like object.
    """
    pass


class ImmutableObjectError(_mgp.ImmutableObjectError):
    """
    Signals modification of an immutable object.
    """
    pass


class ValueConversionError(_mgp.ValueConversionError):
    """
    Signals that the conversion failed between python and cypher values.
    """
    pass


class SerializationError(_mgp.SerializationError):
    """
    Signals serialization error caused by concurrent modifications from
    different transactions.
    """
    pass


class Label:
    """Label of a Vertex."""
    __slots__ = ('_name',)

    def __init__(self, name: str):
        self._name = name

    @property
    def name(self) -> str:
        return self._name

    def __eq__(self, other) -> bool:
        if isinstance(other, Label):
            return self._name == other.name
        if isinstance(other, str):
            return self._name == other
        return NotImplemented


# Named property value of a Vertex or an Edge.
# It would be better to use typing.NamedTuple with typed fields, but that is
# not available in Python 3.5.
Property = namedtuple('Property', ('name', 'value'))


class Properties:
    """
    A collection of properties either on a Vertex or an Edge.
    """
    __slots__ = ('_vertex_or_edge', '_len',)

    def __init__(self, vertex_or_edge):
        if not isinstance(vertex_or_edge, (_mgp.Vertex, _mgp.Edge)):
            raise TypeError("Expected '_mgp.Vertex' or '_mgp.Edge', \
                            got {}".format(type(vertex_or_edge)))
        self._len = None
        self._vertex_or_edge = vertex_or_edge

    def __deepcopy__(self, memo):
        # This is the same as the shallow copy, as the underlying C API should
        # not support deepcopy. Besides, it doesn't make much sense to actually
        # copy _mgp.Edge and _mgp.Vertex types as they are actually references
        # to graph elements and not proper values.
        return Properties(self._vertex_or_edge)

    def get(self, property_name: str, default=None) -> object:
        """
        Get the value of a property with the given name or return default.

        Raise InvalidContextError.
        Raise UnableToAllocateError if unable to allocate a mgp.Value.
        Raise DeletedObjectError if the object has been deleted.
        """
        if not self._vertex_or_edge.is_valid():
            raise InvalidContextError()
        try:
            return self[property_name]
        except KeyError:
            return default

    def set(self, property_name: str, value: object) -> None:
        """
        Set the value of the property. When the value is `None`, then the
        property is removed.

        Raise UnableToAllocateError if unable to allocate memory for storing
        the property.
        Raise ImmutableObjectError if the object is immutable.
        Raise DeletedObjectError if the ojbect has been deleted.
        Raise SerializationError if the object has been modified by another
        transaction.
        Raise ValueConversionError if `value` is vertex, edge or path.
        """
        self[property_name] = value

    def items(self) -> typing.Iterable[Property]:
        """
        Iterate over the properties.

        Doesn’t return a dynamic view of the properties, but copies the
        current properties.

        Raise InvalidContextError.
        Raise UnableToAllocateError if unable to allocate an iterator.
        Raise DeletedObjectError if the object has been deleted.
        """
        if not self._vertex_or_edge.is_valid():
            raise InvalidContextError()
        properties_it = self._vertex_or_edge.iter_properties()
        prop = properties_it.get()
        while prop is not None:
            yield Property(*prop)
            if not self._vertex_or_edge.is_valid():
                raise InvalidContextError()
            prop = properties_it.next()

    def keys(self) -> typing.Iterable[str]:
        """
        Iterate over property names.

        Doesn’t return a dynamic view of the property names, but copies the
        name of the current properties.

        Raise InvalidContextError.
        Raise UnableToAllocateError if unable to allocate an iterator.
        Raise DeletedObjectError if the object has been deleted.
        """
        if not self._vertex_or_edge.is_valid():
            raise InvalidContextError()
        for item in self.items():
            yield item.name

    def values(self) -> typing.Iterable[object]:
        """
        Iterate over property values.

        Doesn’t return a dynamic view of the property values, but copies the
        value of the current properties.

        Raise InvalidContextError.
        Raise UnableToAllocateError if unable to allocate an iterator.
        Raise DeletedObjectError if the object has been deleted.
        """
        if not self._vertex_or_edge.is_valid():
            raise InvalidContextError()
        for item in self.items():
            yield item.value

    def __len__(self) -> int:
        """
        Get the number of properties.

        Raise InvalidContextError.
        Raise UnableToAllocateError if unable to allocate an iterator.
        Raise DeletedObjectError if the object has been deleted.
        """
        if not self._vertex_or_edge.is_valid():
            raise InvalidContextError()
        if self._len is None:
            self._len = sum(1 for item in self.items())
        return self._len

    def __iter__(self) -> typing.Iterable[str]:
        """
        Iterate over property names.

        Raise InvalidContextError.
        Raise UnableToAllocateError if unable to allocate an iterator.
        Raise DeletedObjectError if the object has been deleted.
        """
        if not self._vertex_or_edge.is_valid():
            raise InvalidContextError()
        for item in self.items():
            yield item.name

    def __getitem__(self, property_name: str) -> object:
        """
        Get the value of a property with the given name or raise KeyError.

        Raise InvalidContextError.
        Raise UnableToAllocateError if unable to allocate a mgp.Value.
        Raise DeletedObjectError if the object has been deleted.
        """
        if not self._vertex_or_edge.is_valid():
            raise InvalidContextError()
        prop = self._vertex_or_edge.get_property(property_name)
        if prop is None:
            raise KeyError()
        return prop

    def __setitem__(self, property_name: str, value: object) -> None:
        """
        Set the value of the property. When the value is `None`, then the
        property is removed.

        Raise UnableToAllocateError if unable to allocate memory for storing
        the property.
        Raise ImmutableObjectError if the object is immutable.
        Raise DeletedObjectError if the ojbect has been deleted.
        Raise SerializationError if the object has been modified by another
        transaction.
        Raise ValueConversionError if `value` is vertex, edge or path.
        """
        if not self._vertex_or_edge.is_valid():
            raise InvalidContextError()

        self._vertex_or_edge.set_property(property_name, value)

    def __contains__(self, property_name: str) -> bool:
        """
        Check if there is a property with the given name.

        Raise InvalidContextError.
        Raise UnableToAllocateError if unable to allocate a mgp.Value.
        Raise DeletedObjectError if the object has been deleted.
        """
        if not self._vertex_or_edge.is_valid():
            raise InvalidContextError()
        try:
            _ = self[property_name]
            return True
        except KeyError:
            return False


class EdgeType:
    """Type of an Edge."""
    __slots__ = ('_name',)

    def __init__(self, name):
        self._name = name

    @property
    def name(self) -> str:
        return self._name

    def __eq__(self, other) -> bool:
        if isinstance(other, EdgeType):
            return self.name == other.name
        if isinstance(other, str):
            return self.name == other
        return NotImplemented


if sys.version_info >= (3, 5, 2):
    EdgeId = typing.NewType('EdgeId', int)
else:
    EdgeId = int


class Edge:
    """Edge in the graph database.

    Access to an Edge is only valid during a single execution of a procedure in
    a query. You should not globally store an instance of an Edge. Using an
    invalid Edge instance will raise InvalidContextError.
    """
    __slots__ = ('_edge',)

    def __init__(self, edge):
        if not isinstance(edge, _mgp.Edge):
            raise TypeError(
                "Expected '_mgp.Edge', got '{}'".format(type(edge)))
        self._edge = edge

    def __deepcopy__(self, memo):
        # This is the same as the shallow copy, because we want to share the
        # underlying C struct. Besides, it doesn't make much sense to actually
        # copy _mgp.Edge as that is actually a reference to a graph element
        # and not a proper value.
        return Edge(self._edge)

    def is_valid(self) -> bool:
        """Return True if `self` is in valid context and may be used."""
        return self._edge.is_valid()

    def underlying_graph_is_mutable(self) -> bool:
        """Return True if the edge can be modified."""
        if not self.is_valid():
            raise InvalidContextError()
        return self._edge.underlying_graph_is_mutable()

    @property
    def id(self) -> EdgeId:
        """
        Get the ID of the edge.

        Raise InvalidContextError.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return self._edge.get_id()

    @property
    def type(self) -> EdgeType:
        """
        Get the type of the edge.

        Raise InvalidContextError.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return EdgeType(self._edge.get_type_name())

    @property
    def from_vertex(self) -> 'Vertex':
        """
        Get the source vertex.

        Raise InvalidContextError.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return Vertex(self._edge.from_vertex())

    @property
    def to_vertex(self) -> 'Vertex':
        """
        Get the destination vertex.

        Raise InvalidContextError.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return Vertex(self._edge.to_vertex())

    @property
    def properties(self) -> Properties:
        """
        Get the properties of the edge.

        Raise InvalidContextError.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return Properties(self._edge)

    def __eq__(self, other) -> bool:
        """Raise InvalidContextError."""
        if not self.is_valid():
            raise InvalidContextError()
        if not isinstance(other, Edge):
            return NotImplemented
        return self._edge == other._edge

    def __hash__(self) -> int:
        return hash(self.id)


if sys.version_info >= (3, 5, 2):
    VertexId = typing.NewType('VertexId', int)
else:
    VertexId = int


class Vertex:
    """Vertex in the graph database.

    Access to a Vertex is only valid during a single execution of a procedure
    in a query. You should not globally store an instance of a Vertex. Using an
    invalid Vertex instance will raise InvalidContextError.
    """
    __slots__ = ('_vertex',)

    def __init__(self, vertex):
        if not isinstance(vertex, _mgp.Vertex):
            raise TypeError(
                "Expected '_mgp.Vertex', got '{}'".format(type(vertex)))
        self._vertex = vertex

    def __deepcopy__(self, memo):
        # This is the same as the shallow copy, because we want to share the
        # underlying C struct. Besides, it doesn't make much sense to actually
        # copy _mgp.Vertex as that is actually a reference to a graph element
        # and not a proper value.
        return Vertex(self._vertex)

    def is_valid(self) -> bool:
        """Return True if `self` is in valid context and may be used."""
        return self._vertex.is_valid()

    def underlying_graph_is_mutable(self) -> bool:
        """Return True if the vertex can be modified."""
        if not self.is_valid():
            raise InvalidContextError()
        return self._vertex.underlying_graph_is_mutable()

    @property
    def id(self) -> VertexId:
        """
        Get the ID of the vertex.

        Raise InvalidContextError.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return self._vertex.get_id()

    @property
    def labels(self) -> typing.Tuple[Label]:
        """
        Get the labels of the vertex.

        Raise InvalidContextError.
        Raise OutOfRangeError if some of the labels are removed while
        collecting the labels.
        Raise DeletedObjectError if `self` has been deleted.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return tuple(Label(self._vertex.label_at(i))
                     for i in range(self._vertex.labels_count()))

    def add_label(self, label: str) -> None:
        """
        Add the label to the vertex.

        Raise InvalidContextError.
        Raise UnableToAllocateError if unable to allocate memory for storing
        the label.
        Raise ImmutableObjectError if `self` is immutable.
        Raise DeletedObjectError if `self` has been deleted.
        Raise SerializationError if `self` has been modified by another
        transaction.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return self._vertex.add_label(label)

    def remove_label(self, label: str) -> None:
        """
        Remove the label from the vertex.

        Raise InvalidContextError.
        Raise ImmutableObjectError if `self` is immutable.
        Raise DeletedObjectError if `self` has been deleted.
        Raise SerializationError if `self` has been modified by another
        transaction.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return self._vertex.remove_label(label)

    @property
    def properties(self) -> Properties:
        """
        Get the properties of the vertex.

        Raise InvalidContextError.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return Properties(self._vertex)

    @property
    def in_edges(self) -> typing.Iterable[Edge]:
        """
        Iterate over inbound edges of the vertex.

        Doesn’t return a dynamic view of the edges, but copies the
        current inbound edges.

        Raise InvalidContextError.
        Raise UnableToAllocateError if unable to allocate an iterator.
        Raise DeletedObjectError if `self` has been deleted.
        """
        if not self.is_valid():
            raise InvalidContextError()
        edges_it = self._vertex.iter_in_edges()
        edge = edges_it.get()
        while edge is not None:
            yield Edge(edge)
            if not self.is_valid():
                raise InvalidContextError()
            edge = edges_it.next()

    @property
    def out_edges(self) -> typing.Iterable[Edge]:
        """
        Iterate over outbound edges of the vertex.

        Doesn’t return a dynamic view of the edges, but copies the
        current outbound edges.

        Raise InvalidContextError.
        Raise UnableToAllocateError if unable to allocate an iterator.
        Raise DeletedObjectError if `self` has been deleted.
        """
        if not self.is_valid():
            raise InvalidContextError()
        edges_it = self._vertex.iter_out_edges()
        edge = edges_it.get()
        while edge is not None:
            yield Edge(edge)
            if not self.is_valid():
                raise InvalidContextError()
            edge = edges_it.next()

    def __eq__(self, other) -> bool:
        """Raise InvalidContextError"""
        if not self.is_valid():
            raise InvalidContextError()
        if not isinstance(other, Vertex):
            return NotImplemented
        return self._vertex == other._vertex

    def __hash__(self) -> int:
        return hash(self.id)


class Path:
    """Path containing Vertex and Edge instances."""
    __slots__ = ('_path', '_vertices', '_edges')

    def __init__(self, starting_vertex_or_path: typing.Union[_mgp.Path, Vertex]):
        """Initialize with a starting Vertex.

        Raise InvalidContextError if passed in Vertex is invalid.
        Raise UnableToAllocateError if cannot allocate a path.
        """
        # We cache calls to `vertices` and `edges`, so as to avoid needless
        # allocations at the C level.
        self._vertices = None
        self._edges = None
        # Accepting _mgp.Path is just for internal usage.
        if isinstance(starting_vertex_or_path, _mgp.Path):
            self._path = starting_vertex_or_path
        elif isinstance(starting_vertex_or_path, Vertex):
            vertex = starting_vertex_or_path._vertex
            if not vertex.is_valid():
                raise InvalidContextError()
            self._path = _mgp.Path.make_with_start(vertex)
        else:
            raise TypeError("Expected '_mgp.Vertex' or '_mgp.Path', got '{}'"
                            .format(type(starting_vertex_or_path)))

    def __copy__(self):
        if not self.is_valid():
            raise InvalidContextError()
        assert len(self.vertices) >= 1
        path = Path(self.vertices[0])
        for e in self.edges:
            path.expand(e)
        return path

    def __deepcopy__(self, memo):
        try:
            return Path(memo[id(self._path)])
        except KeyError:
            pass
        # This is the same as the shallow copy, as the underlying C API should
        # not support deepcopy. Besides, it doesn't make much sense to actually
        # copy _mgp.Edge and _mgp.Vertex types as they are actually references
        # to graph elements and not proper values.
        path = self.__copy__()
        memo[id(self._path)] = path._path
        return path

    def is_valid(self) -> bool:
        return self._path.is_valid()

    def expand(self, edge: Edge):
        """Append an edge continuing from the last vertex on the path.

        The last vertex on the path will become the other endpoint of the given
        edge, as continued from the current last vertex.

        Raise InvalidContextError if using an invalid Path instance or if
        passed in edge is invalid.
        Raise LogicErrorError if the current last vertex in the path is not
        part of the given edge.
        Raise UnableToAllocateError if unable to allocate memory for path
        extension.
        """
        if not isinstance(edge, Edge):
            raise TypeError(
                "Expected '_mgp.Edge', got '{}'".format(type(edge)))
        if not self.is_valid() or not edge.is_valid():
            raise InvalidContextError()
        self._path.expand(edge._edge)
        # Invalidate our cached tuples
        self._vertices = None
        self._edges = None

    @property
    def vertices(self) -> typing.Tuple[Vertex, ...]:
        """
        Vertices ordered from the start to the end of the path.

        Raise InvalidContextError if using an invalid Path instance.
        """
        if not self.is_valid():
            raise InvalidContextError()
        if self._vertices is None:
            num_vertices = self._path.size() + 1
            self._vertices = tuple(Vertex(self._path.vertex_at(i))
                                   for i in range(num_vertices))
        return self._vertices

    @property
    def edges(self) -> typing.Tuple[Edge, ...]:
        """
        Edges ordered from the start to the end of the path.

        Raise InvalidContextError if using an invalid Path instance.
        """
        if not self.is_valid():
            raise InvalidContextError()
        if self._edges is None:
            num_edges = self._path.size()
            self._edges = tuple(Edge(self._path.edge_at(i))
                                for i in range(num_edges))
        return self._edges


class Record:
    """Represents a record of resulting field values."""
    __slots__ = ('fields',)

    def __init__(self, **kwargs):
        """Initialize with name=value fields in kwargs."""
        self.fields = kwargs


class Vertices:
    """Iterable over vertices in a graph."""
    __slots__ = ('_graph', '_len')

    def __init__(self, graph):
        if not isinstance(graph, _mgp.Graph):
            raise TypeError(
                "Expected '_mgp.Graph', got '{}'".format(type(graph)))
        self._graph = graph
        self._len = None

    def __deepcopy__(self, memo):
        # This is the same as the shallow copy, because we want to share the
        # underlying C struct. Besides, it doesn't make much sense to actually
        # copy _mgp.Graph as that always references the whole graph state.
        return Vertices(self._graph)

    def is_valid(self) -> bool:
        """Return True if `self` is in valid context and may be used."""
        return self._graph.is_valid()

    def __iter__(self) -> typing.Iterable[Vertex]:
        """
        Iterate over vertices.

        Raise InvalidContextError if context is invalid.
        Raise UnableToAllocateError if unable to allocate an iterator or
        a vertex.
        """
        if not self.is_valid():
            raise InvalidContextError()
        vertices_it = self._graph.iter_vertices()
        vertex = vertices_it.get()
        while vertex is not None:
            yield Vertex(vertex)
            if not self.is_valid():
                raise InvalidContextError()
            vertex = vertices_it.next()

    def __contains__(self, vertex):
        """
        Raise UnableToAllocateError if unable to allocate the vertex.
        """
        try:
            _ = self._graph.get_vertex_by_id(vertex.id)
            return True
        except IndexError:
            return False

    def __len__(self):
        """
        Get the number of vertices.

        Raise InvalidContextError if context is invalid.
        Raise UnableToAllocateError if unable to allocate an iterator or
        a vertex.
        """
        if not self._len:
            self._len = sum(1 for _ in self)
        return self._len


class Graph:
    """State of the graph database in current ProcCtx."""
    __slots__ = ('_graph',)

    def __init__(self, graph):
        if not isinstance(graph, _mgp.Graph):
            raise TypeError(
                "Expected '_mgp.Graph', got '{}'".format(type(graph)))
        self._graph = graph

    def __deepcopy__(self, memo):
        # This is the same as the shallow copy, because we want to share the
        # underlying C struct. Besides, it doesn't make much sense to actually
        # copy _mgp.Graph as that always references the whole graph state.
        return Graph(self._graph)

    def is_valid(self) -> bool:
        """Return True if `self` is in valid context and may be used."""
        return self._graph.is_valid()

    def get_vertex_by_id(self, vertex_id: VertexId) -> Vertex:
        """
        Return the Vertex corresponding to given vertex_id from the graph.

        Access to a Vertex is only valid during a single execution of a
        procedure in a query. You should not globally store the returned
        Vertex.

        Raise IndexError if unable to find the given vertex_id.
        Raise InvalidContextError if context is invalid.
        """
        if not self.is_valid():
            raise InvalidContextError()
        vertex = self._graph.get_vertex_by_id(vertex_id)
        return Vertex(vertex)

    @property
    def vertices(self) -> Vertices:
        """
        All vertices in the graph.

        Access to a Vertex is only valid during a single execution of a
        procedure in a query. You should not globally store the returned Vertex
        instances.

        Raise InvalidContextError if context is invalid.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return Vertices(self._graph)

    def is_mutable(self) -> bool:
        """
        Return True if `self` represents a mutable graph, thus it can be
        used to modify vertices and edges.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return self._graph.is_mutable()

    def create_vertex(self) -> Vertex:
        """
        Create a vertex.

        Raise ImmutableObjectError if `self` is immutable.
        Raise UnableToAllocateError if unable to allocate a vertex.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return Vertex(self._graph.create_vertex())

    def delete_vertex(self, vertex: Vertex) -> None:
        """
        Delete a vertex.

        Raise ImmutableObjectError if `self` is immutable.
        Raise LogicErrorError if `vertex` has edges.
        Raise SerializationError if `vertex` has been modified by
        another transaction.
        """
        if not self.is_valid():
            raise InvalidContextError()
        self._graph.delete_vertex(vertex._vertex)

    def detach_delete_vertex(self, vertex: Vertex) -> None:
        """
        Delete a vertex and all of its edges.

        Raise ImmutableObjectError if `self` is immutable.
        Raise SerializationError if `vertex` has been modified by
        another transaction.
        """
        if not self.is_valid():
            raise InvalidContextError()
        self._graph.detach_delete_vertex(vertex._vertex)

    def create_edge(self, from_vertex: Vertex, to_vertex: Vertex,
                    edge_type: EdgeType) -> None:
        """
        Create an edge.

        Raise ImmutableObjectError if `self ` is immutable.
        Raise UnableToAllocateError if unable to allocate an edge.
        Raise DeletedObjectError if `from_vertex` or `to_vertex` has
        been deleted.
        Raise SerializationError if `from_vertex` or `to_vertex` has
        been modified by another transaction.
        """
        if not self.is_valid():
            raise InvalidContextError()
        return Edge(self._graph.create_edge(from_vertex._vertex,
                                            to_vertex._vertex, edge_type.name))

    def delete_edge(self, edge: Edge) -> None:
        """
        Delete an edge.


        Raise ImmutableObjectError if `self` is immutable.
        Raise SerializationError if `edge`, its source or destination
        vertex has been modified by another transaction.
        """
        if not self.is_valid():
            raise InvalidContextError()
        self._graph.delete_edge(edge._edge)


class AbortError(Exception):
    """Signals that the procedure was asked to abort its execution."""
    pass


class ProcCtx:
    """Context of a procedure being executed.

    Access to a ProcCtx is only valid during a single execution of a procedure
    in a query. You should not globally store a ProcCtx instance.
    """
    __slots__ = ('_graph',)

    def __init__(self, graph):
        if not isinstance(graph, _mgp.Graph):
            raise TypeError(
                "Expected '_mgp.Graph', got '{}'".format(type(graph)))
        self._graph = Graph(graph)

    def is_valid(self) -> bool:
        return self._graph.is_valid()

    @property
    def graph(self) -> Graph:
        """Raise InvalidContextError if context is invalid."""
        if not self.is_valid():
            raise InvalidContextError()
        return self._graph

    def must_abort(self) -> bool:
        if not self.is_valid():
            raise InvalidContextError()
        return self._graph._graph.must_abort()

    def check_must_abort(self):
        if self.must_abort():
            raise AbortError


# Additional typing support

Number = typing.Union[int, float]

Map = typing.Union[dict, Edge, Vertex]

Date = datetime.date

LocalTime = datetime.time

LocalDateTime = datetime.datetime

Duration = datetime.timedelta

Any = typing.Union[bool, str, Number, Map, Path,
                   list, Date, LocalTime, LocalDateTime, Duration]

List = typing.List

Nullable = typing.Optional


class UnsupportedTypingError(Exception):
    """Signals a typing annotation is not supported as a _mgp.CypherType."""

    def __init__(self, type_):
        super().__init__("Unsupported typing annotation '{}'".format(type_))


def _typing_to_cypher_type(type_):
    """Convert typing annotation to a _mgp.CypherType instance."""
    simple_types = {
        typing.Any: _mgp.type_nullable(_mgp.type_any()),
        object: _mgp.type_nullable(_mgp.type_any()),
        list: _mgp.type_list(_mgp.type_nullable(_mgp.type_any())),
        Any: _mgp.type_any(),
        bool: _mgp.type_bool(),
        str: _mgp.type_string(),
        int: _mgp.type_int(),
        float: _mgp.type_float(),
        Number: _mgp.type_number(),
        Map: _mgp.type_map(),
        Vertex: _mgp.type_node(),
        Edge: _mgp.type_relationship(),
        Path: _mgp.type_path(),
        Date: _mgp.type_date(),
        LocalTime: _mgp.type_local_time(),
        LocalDateTime: _mgp.type_local_date_time(),
        Duration: _mgp.type_duration()
    }
    try:
        return simple_types[type_]
    except KeyError:
        pass
    if sys.version_info >= (3, 8):
        complex_type = typing.get_origin(type_)
        type_args = typing.get_args(type_)
        if complex_type == typing.Union:
            # If we have a Union with NoneType inside, it means we are building
            # a nullable type.
            # isinstance doesn't work here because subscripted generics cannot
            # be used with class and instance checks. type comparison should be
            # fine because subclasses are not used.
            if type(None) in type_args:
                types = tuple(t for t in type_args if t is not type(None))  # noqa E721
                if len(types) == 1:
                    type_arg, = types
                else:
                    # We cannot do typing.Union[*types], so do the equivalent
                    # with __getitem__ which does not even need arg unpacking.
                    type_arg = typing.Union.__getitem__(types)
                return _mgp.type_nullable(_typing_to_cypher_type(type_arg))
        elif complex_type == list:
            type_arg, = type_args
            return _mgp.type_list(_typing_to_cypher_type(type_arg))
        raise UnsupportedTypingError(type_)
    else:
        # We cannot get to type args in any reliable way prior to 3.8, but we
        # still want to support typing.Optional and typing.List, so just parse
        # their string representations. Hopefully, that is always pretty
        # printed the same way. `typing.List[type]` is printed as such, while
        # `typing.Optional[type]` is printed as 'typing.Union[type, NoneType]'
        def parse_type_args(type_as_str):
            return tuple(map(str.strip,
                             type_as_str[type_as_str.index('[') + 1: -1].split(',')))

        def fully_qualified_name(cls):
            if cls.__module__ is None or cls.__module__ == 'builtins':
                return cls.__name__
            return cls.__module__ + '.' + cls.__name__

        def get_simple_type(type_as_str):
            for simple_type, cypher_type in simple_types.items():
                if type_as_str == str(simple_type):
                    return cypher_type
                # Fallback to comparing to __name__ if it exits. This handles
                # the cases like when we have 'object' which is
                # `object.__name__`, but `str(object)` is "<class 'object'>"
                try:
                    if type_as_str == fully_qualified_name(simple_type):
                        return cypher_type
                except AttributeError:
                    pass

        def parse_typing(type_as_str):
            if type_as_str.startswith('typing.Union'):
                type_args_as_str = parse_type_args(type_as_str)
                none_type_as_str = type(None).__name__
                if none_type_as_str in type_args_as_str:
                    types = tuple(
                        t for t in type_args_as_str if t != none_type_as_str)
                    if len(types) == 1:
                        type_arg_as_str, = types
                    else:
                        type_arg_as_str = 'typing.Union[' + \
                            ', '.join(types) + ']'
                    simple_type = get_simple_type(type_arg_as_str)
                    if simple_type is not None:
                        return _mgp.type_nullable(simple_type)
                    return _mgp.type_nullable(parse_typing(type_arg_as_str))
            elif type_as_str.startswith('typing.List'):
                type_arg_as_str = parse_type_args(type_as_str)

                if len(type_arg_as_str) > 1:
                    # Nested object could be a type consisting of a list of types (e.g. mgp.Map)
                    # so we need to join the parts.
                    type_arg_as_str = ', '.join(type_arg_as_str)
                else:
                    type_arg_as_str = type_arg_as_str[0]

                simple_type = get_simple_type(type_arg_as_str)
                if simple_type is not None:
                    return _mgp.type_list(simple_type)
                return _mgp.type_list(parse_typing(type_arg_as_str))
            raise UnsupportedTypingError(type_)

        return parse_typing(str(type_))


# Procedure registration

class Deprecated:
    """Annotate a resulting Record's field as deprecated."""
    __slots__ = ('field_type',)

    def __init__(self, type_):
        self.field_type = type_


def raise_if_does_not_meet_requirements(func: typing.Callable[..., Record]):
    if not callable(func):
        raise TypeError("Expected a callable object, got an instance of '{}'"
                        .format(type(func)))
    if inspect.iscoroutinefunction(func):
        raise TypeError("Callable must not be 'async def' function")
    if sys.version_info >= (3, 6):
        if inspect.isasyncgenfunction(func):
            raise TypeError("Callable must not be 'async def' function")
    if inspect.isgeneratorfunction(func):
        raise NotImplementedError("Generator functions are not supported")


def _register_proc(func: typing.Callable[..., Record],
                   is_write: bool):
    raise_if_does_not_meet_requirements(func)
    register_func = (
        _mgp.Module.add_write_procedure if is_write
        else _mgp.Module.add_read_procedure)
    sig = inspect.signature(func)
    params = tuple(sig.parameters.values())
    if params and params[0].annotation is ProcCtx:
        @wraps(func)
        def wrapper(graph, args):
            return func(ProcCtx(graph), *args)
        params = params[1:]
        mgp_proc = register_func(_mgp._MODULE, wrapper)
    else:
        @wraps(func)
        def wrapper(graph, args):
            return func(*args)
        mgp_proc = register_func(_mgp._MODULE, wrapper)
    for param in params:
        name = param.name
        type_ = param.annotation
        if type_ is param.empty:
            type_ = object
        cypher_type = _typing_to_cypher_type(type_)
        if param.default is param.empty:
            mgp_proc.add_arg(name, cypher_type)
        else:
            mgp_proc.add_opt_arg(name, cypher_type, param.default)
    if sig.return_annotation is not sig.empty:
        record = sig.return_annotation
        if not isinstance(record, Record):
            raise TypeError("Expected '{}' to return 'mgp.Record', got '{}'"
                            .format(func.__name__, type(record)))
        for name, type_ in record.fields.items():
            if isinstance(type_, Deprecated):
                cypher_type = _typing_to_cypher_type(type_.field_type)
                mgp_proc.add_deprecated_result(name, cypher_type)
            else:
                mgp_proc.add_result(name, _typing_to_cypher_type(type_))
    return func


def read_proc(func: typing.Callable[..., Record]):
    """
    Register `func` as a read-only procedure of the current module.

    `read_proc` is meant to be used as a decorator function to register module
    procedures. The registered `func` needs to be a callable which optionally
    takes `ProcCtx` as the first argument. Other arguments of `func` will be
    bound to values passed in the cypherQuery. The full signature of `func`
    needs to be annotated with types. The return type must be
    `Record(field_name=type, ...)` and the procedure must produce either a
    complete Record or None. To mark a field as deprecated, use
    `Record(field_name=Deprecated(type), ...)`. Multiple records can be
    produced by returning an iterable of them. Registering generator functions
    is currently not supported.

    Example usage.

    ```
    import mgp

    @mgp.read_proc
    def procedure(context: mgp.ProcCtx,
                  required_arg: mgp.Nullable[mgp.Any],
                  optional_arg: mgp.Nullable[mgp.Any] = None
                  ) -> mgp.Record(result=str, args=list):
        args = [required_arg, optional_arg]
        # Multiple rows can be produced by returning an iterable of mgp.Record
        return mgp.Record(args=args, result='Hello World!')
    ```

    The example procedure above returns 2 fields: `args` and `result`.
      * `args` is a copy of arguments passed to the procedure.
      * `result` is the result of this procedure, a "Hello World!" string.
    Any errors can be reported by raising an Exception.

    The procedure can be invoked in openCypher using the following calls:
      CALL example.procedure(1, 2) YIELD args, result;
      CALL example.procedure(1) YIELD args, result;
    Naturally, you may pass in different arguments or yield less fields.
    """
    return _register_proc(func, False)


def write_proc(func: typing.Callable[..., Record]):
    """
    Register `func` as a writeable procedure of the current module.

    `write_proc` is meant to be used as a decorator function to register module
    procedures. The registered `func` needs to be a callable which optionally
    takes `ProcCtx` as the first argument. Other arguments of `func` will be
    bound to values passed in the cypherQuery. The full signature of `func`
    needs to be annotated with types. The return type must be
    `Record(field_name=type, ...)` and the procedure must produce either a
    complete Record or None. To mark a field as deprecated, use
    `Record(field_name=Deprecated(type), ...)`. Multiple records can be
    produced by returning an iterable of them. Registering generator functions
    is currently not supported.

    Example usage.

    ```
    import mgp

    @mgp.write_proc
    def procedure(context: mgp.ProcCtx,
                required_arg: str,
                optional_arg: mgp.Nullable[str] = None
                ) -> mgp.Record(result=mgp.Vertex):
        vertex = context.graph.create_vertex()
        vertex_properties = vertex.properties
        vertex_properties["required_arg"] = required_arg
        if optional_arg is not None:
            vertex_properties["optional_arg"] = optional_arg

        return mgp.Record(result=vertex)
    ```

    The example procedure above returns  a newly created vertex which has
    at most 2 properties:
      * `required_arg` is always present and its value is the first
        argument of the procedure.
      * `optional_arg` is present if the second argument of the procedure
        is not `null`.
    Any errors can be reported by raising an Exception.

    The procedure can be invoked in openCypher using the following calls:
      CALL example.procedure("property value", "another one") YIELD result;
      CALL example.procedure("single argument") YIELD result;
    Naturally, you may pass in different arguments.
    """
    return _register_proc(func, True)


class InvalidMessageError(Exception):
    """
    Signals using a message instance outside of the registered transformation.
    """
    pass


class Message:
    """Represents a message from a stream."""
    __slots__ = ('_message',)

    def __init__(self, message):
        if not isinstance(message, _mgp.Message):
            raise TypeError(
                "Expected '_mgp.Message', got '{}'".format(type(message)))
        self._message = message

    def __deepcopy__(self, memo):
        # This is the same as the shallow copy, because we want to share the
        # underlying C struct. Besides, it doesn't make much sense to actually
        # copy _mgp.Messages as that always references all the messages.
        return Message(self._message)

    def is_valid(self) -> bool:
        """Return True if `self` is in valid context and may be used."""
        return self._message.is_valid()

    def payload(self) -> bytes:
        if not self.is_valid():
            raise InvalidMessageError()
        return self._message.payload()

    def topic_name(self) -> str:
        if not self.is_valid():
            raise InvalidMessageError()
        return self._message.topic_name()

    def key(self) -> bytes:
        if not self.is_valid():
            raise InvalidMessageError()
        return self._message.key()

    def timestamp(self) -> int:
        if not self.is_valid():
            raise InvalidMessageError()
        return self._message.timestamp()


class InvalidMessagesError(Exception):
    """Signals using a messages instance outside of the registered transformation."""
    pass


class Messages:
    """Represents a list of messages from a stream."""
    __slots__ = ('_messages',)

    def __init__(self, messages):
        if not isinstance(messages, _mgp.Messages):
            raise TypeError(
                "Expected '_mgp.Messages', got '{}'".format(type(messages)))
        self._messages = messages

    def __deepcopy__(self, memo):
        # This is the same as the shallow copy, because we want to share the
        # underlying C struct. Besides, it doesn't make much sense to actually
        # copy _mgp.Messages as that always references all the messages.
        return Messages(self._messages)

    def is_valid(self) -> bool:
        """Return True if `self` is in valid context and may be used."""
        return self._messages.is_valid()

    def message_at(self, id: int) -> Message:
        """Raise InvalidMessagesError if context is invalid."""
        if not self.is_valid():
            raise InvalidMessagesError()
        return Message(self._messages.message_at(id))

    def total_messages(self) -> int:
        """Raise InvalidContextError if context is invalid."""
        if not self.is_valid():
            raise InvalidMessagesError()
        return self._messages.total_messages()


class TransCtx:
    """Context of a transformation being executed.

    Access to a TransCtx is only valid during a single execution of a transformation.
    You should not globally store a TransCtx instance.
    """
    __slots__ = ('_graph')

    def __init__(self, graph):
        if not isinstance(graph, _mgp.Graph):
            raise TypeError(
                "Expected '_mgp.Graph', got '{}'".format(type(graph)))
        self._graph = Graph(graph)

    def is_valid(self) -> bool:
        return self._graph.is_valid()

    @property
    def graph(self) -> Graph:
        """Raise InvalidContextError if context is invalid."""
        if not self.is_valid():
            raise InvalidContextError()
        return self._graph


def transformation(func: typing.Callable[..., Record]):
    raise_if_does_not_meet_requirements(func)
    sig = inspect.signature(func)
    params = tuple(sig.parameters.values())
    if not params or not params[0].annotation is Messages:
        if not len(params) == 2 or not params[1].annotation is Messages:
            raise NotImplementedError(
                "Valid signatures for transformations are (TransCtx, Messages) or (Messages)")
    if params[0].annotation is TransCtx:
        @wraps(func)
        def wrapper(graph, messages):
            return func(TransCtx(graph), messages)
        _mgp._MODULE.add_transformation(wrapper)
    else:
        @wraps(func)
        def wrapper(graph, messages):
            return func(messages)
        _mgp._MODULE.add_transformation(wrapper)
    return func


def _wrap_exceptions():
    def wrap_function(func):
        @wraps(func)
        def wrapped_func(*args, **kwargs):
            try:
                return func(*args, **kwargs)
            except _mgp.UnknownError as e:
                raise UnknownError(e)
            except _mgp.UnableToAllocateError as e:
                raise UnableToAllocateError(e)
            except _mgp.InsufficientBufferError as e:
                raise InsufficientBufferError(e)
            except _mgp.OutOfRangeError as e:
                raise OutOfRangeError(e)
            except _mgp.LogicErrorError as e:
                raise LogicErrorError(e)
            except _mgp.DeletedObjectError as e:
                raise DeletedObjectError(e)
            except _mgp.InvalidArgumentError as e:
                raise InvalidArgumentError(e)
            except _mgp.KeyAlreadyExistsError as e:
                raise KeyAlreadyExistsError(e)
            except _mgp.ImmutableObjectError as e:
                raise ImmutableObjectError(e)
            except _mgp.ValueConversionError as e:
                raise ValueConversionError(e)
            except _mgp.SerializationError as e:
                raise SerializationError(e)
        return wrapped_func

    def wrap_prop_func(func):
        return None if func is None else wrap_function(func)

    def wrap_member_functions(cls: type):
        for name, obj in inspect.getmembers(cls):
            if inspect.isfunction(obj):
                setattr(cls, name, wrap_function(obj))
            elif isinstance(obj, property):
                setattr(cls, name, property(
                    wrap_prop_func(obj.fget),
                    wrap_prop_func(obj.fset),
                    wrap_prop_func(obj.fdel),
                    obj.__doc__))

    def defined_in_this_module(obj: object):
        return getattr(obj, "__module__", "") == __name__

    module = sys.modules[__name__]
    for name, obj in inspect.getmembers(module):
        if not defined_in_this_module(obj):
            continue
        if inspect.isclass(obj):
            wrap_member_functions(obj)
        elif inspect.isfunction(obj) and not name.startswith("_"):
            setattr(module, name, wrap_function(obj))


_wrap_exceptions()
