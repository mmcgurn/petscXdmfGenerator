#include "xdmfSpecification.hpp"
#include <algorithm>
#include <stdexcept>

using namespace xdmfGenerator;

const static std::map<std::string, FieldType> petscTypeLookUpFromFieldType = {{"scalar", SCALAR}, {"vector", VECTOR}, {"tensor", TENSOR}, {"matrix", MATRIX}};
const static std::map<int, FieldType> petscTypeLookUpFromNC = {{1, SCALAR}, {2, VECTOR}, {3, VECTOR}};

void xdmfGenerator::XdmfSpecification::GenerateFieldsFromPetsc(std::vector<FieldDescription>& fields, const std::vector<std::shared_ptr<xdmfGenerator::HdfObject>>& hdfFields,
                                                               xdmfGenerator::FieldLocation location, const std::string& fileName, unsigned long long timeOffset) {
    for (auto& hdfField : hdfFields) {
        FieldDescription description{.name = hdfField->Name(),
                                     .location = {.path = hdfField->Path(), .file = fileName},
                                     .shape = hdfField->Shape(),
                                     .timeOffset = timeOffset,
                                     .componentOffset = 0,
                                     .componentStride = 1,
                                     .componentDimension = 0,
                                     .fieldLocation = location,
                                     .fieldType = FieldType::NONE,
                                     .hasTimeDimension = hdfField->HasAttribute("timestepping") && hdfField->Attribute<int>("timestepping") > 0};

        bool separateIntoComponents = false;

        if (hdfField->HasAttribute("vector_field_type")) {  // it is a cell based field
            auto vector_field_type = hdfField->AttributeString("vector_field_type");
            if (petscTypeLookUpFromFieldType.count(vector_field_type)) {
                description.fieldType = petscTypeLookUpFromFieldType.at(vector_field_type);

                // put a check for 1D scalars in cells.  This is a result in mesh dimensionality reduction
                if ((description.fieldType == VECTOR && description.shape.size() < 3 && location == CELL && description.HasTimeDimension()) ||
                    (description.fieldType == VECTOR && description.shape.size() < 2 && location == CELL && !description.HasTimeDimension())) {
                    description.fieldType = SCALAR;
                }

            } else {
                description.fieldType = NONE;
            }
        } else if (hdfField->HasAttribute("Nc")) {  // it is a particle field
            auto nc = hdfField->Attribute<int>("Nc");
            if (petscTypeLookUpFromNC.count(nc)) {
                description.fieldType = petscTypeLookUpFromNC.at(nc);
            } else if (nc) {
                // If there is nc but not between 1-3 assume it is a packed vector and separate
                description.fieldType = VECTOR;
                separateIntoComponents = true;
            } else {
                description.fieldType = NONE;
            }

        } else {
            throw std::runtime_error("Cannot determine field type for " + description.name);
        }

        if (description.fieldType == SCALAR) {
            // the 1 dimension is left off for scalars, add it back to the shape if the object holds a single vector
            if (description.shape.size() < 3) {
                description.shape.push_back(1);
            } else {
                // This is a component type, a single object that holds multiple types
                separateIntoComponents = true;
            }
        }

        // determine the dimensions from the shape
        description.componentDimension = description.shape.size() > 2 ? description.shape[2] : description.shape[1];

        // If this is a components field, separate into each component
        if (description.fieldType != NONE) {
            if (separateIntoComponents) {
                for (unsigned long long c = 0; c < description.GetDimension(); c++) {
                    // check to see if the component was named in the hdf5 file
                    std::string componentName = description.name + std::to_string(c);
                    const std::string attributeName = "componentName" + std::to_string(c);
                    if (hdfField->HasAttribute(attributeName)) {
                        auto hdfComponentName = hdfField->AttributeString(attributeName);
                        componentName = description.name + "_" + hdfComponentName;
                    }

                    // create a temporary fieldDescription for each component
                    xdmfGenerator::XdmfSpecification::FieldDescription componentFieldDescription{.name = componentName,
                                                                                                 .location = description.location,
                                                                                                 .shape = description.shape,
                                                                                                 .timeOffset = description.timeOffset,
                                                                                                 .componentOffset = static_cast<unsigned long long>(c),
                                                                                                 .componentStride = description.GetDimension(),
                                                                                                 .componentDimension = 1,
                                                                                                 .fieldLocation = description.fieldLocation,
                                                                                                 .fieldType = SCALAR,
                                                                                                 .hasTimeDimension = description.hasTimeDimension};

                    fields.push_back(componentFieldDescription);
                }
            } else {
                fields.push_back(description);
            }
        }
    }
}

std::shared_ptr<XdmfSpecification> xdmfGenerator::XdmfSpecification::FromPetscHdf(std::shared_ptr<xdmfGenerator::HdfObject> rootObject) {
    // store the file name
    auto hdf5File = rootObject->Name();

    // make a new specification
    auto specification = std::make_shared<XdmfSpecification>();

    // petsc hdf5 files may have a root domain (this is often a real mesh (FE/FV))
    std::shared_ptr<xdmfGenerator::HdfObject> geometryObject = FindPetscHdfChild(rootObject, "geometry");
    if (geometryObject) {
        GridCollectionDescription mainGrid;

        // march over each possible topology
        int topologyIndex = 0;
        while (auto topologyObject = FindPetscHdfChild(rootObject, "topology" + GetTopologyPostfix(topologyIndex))) {
            // get the time
            auto time = rootObject->Contains("time") ? rootObject->Get("time")->RawData<double>() : std::vector<double>{-1};

            // add in each time
            for (std::size_t timeIndex = 0; timeIndex < time.size(); timeIndex++) {
                GridDescription gridDescription;
                gridDescription.time = time[timeIndex];

                // store the geometry
                auto verticesObject = geometryObject->Get("vertices");
                gridDescription.geometry.name = verticesObject->Name(), gridDescription.geometry.location.path = verticesObject->Path(), gridDescription.geometry.location.file = hdf5File,
                gridDescription.geometry.shape = verticesObject->Shape(), gridDescription.geometry.fieldLocation = NODE, gridDescription.geometry.fieldType = VECTOR,
                gridDescription.geometry.componentDimension = gridDescription.geometry.shape.size() > 2 ? gridDescription.geometry.shape[2] : gridDescription.geometry.shape.back();

                // check for and get the topology
                if (topologyObject) {
                    auto cellObject = topologyObject->Get("cells");
                    gridDescription.topology.location.path = cellObject->Path();
                    gridDescription.topology.location.file = hdf5File;
                    gridDescription.topology.number = cellObject->Shape()[0];
                    gridDescription.topology.numberCorners = cellObject->Shape()[1];
                    gridDescription.topology.dimension = cellObject->Attribute<unsigned long long>("cell_dim");
                }
                // hybrid topology
                std::shared_ptr<xdmfGenerator::HdfObject> hybridTopologyObject = FindPetscHdfChild(rootObject, "hybrid_topology");
                if (hybridTopologyObject) {
                    auto cellObject = topologyObject->Get("hcells");
                    gridDescription.hybridTopology.location.path = cellObject->Path();
                    gridDescription.hybridTopology.location.file = hdf5File;
                    gridDescription.hybridTopology.number = cellObject->Shape()[0];
                    gridDescription.hybridTopology.numberCorners = cellObject->Shape()[1];
                }

                // get the vertex fields and map into a vertex map
                if (rootObject->Contains("vertex_fields" + GetTopologyPostfix(topologyIndex))) {
                    GenerateFieldsFromPetsc(gridDescription.fields, rootObject->Get("vertex_fields" + GetTopologyPostfix(topologyIndex))->Items(), NODE, hdf5File, timeIndex);
                }
                if (rootObject->Contains("cell_fields" + GetTopologyPostfix(topologyIndex))) {
                    GenerateFieldsFromPetsc(gridDescription.fields, rootObject->Get("cell_fields" + GetTopologyPostfix(topologyIndex))->Items(), CELL, hdf5File, timeIndex);
                }

                mainGrid.grids[timeIndex].push_back(gridDescription);
            }
            ++topologyIndex;
        }

        // add to the list of grids
        specification->gridsCollections.push_back(mainGrid);
    }

    // check for particles
    if (rootObject->Contains("particles") || rootObject->Contains("particle_fields")) {
        GridCollectionDescription particleGrid;
        particleGrid.name = "particle_domain";

        // get the time
        auto time = rootObject->Contains("time") ? rootObject->Get("time")->RawData<double>() : std::vector<double>{-1};

        for (std::size_t timeIndex = 0; timeIndex < time.size(); timeIndex++) {
            GridDescription gridDescription;
            gridDescription.time = time[timeIndex];

            // add in any other fields
            if (rootObject->Contains("particle_fields")) {
                GenerateFieldsFromPetsc(gridDescription.fields, rootObject->Get("particle_fields")->Items(), NODE, hdf5File, timeIndex);
            }

            if (rootObject->Contains("particles")) {
                std::shared_ptr<xdmfGenerator::HdfObject> geometryObjectLocal = rootObject->Get("particles")->Get("coordinates");
                // store the geometry
                gridDescription.geometry.name = geometryObjectLocal->Name(), gridDescription.geometry.location.path = geometryObjectLocal->Path(), gridDescription.geometry.location.file = hdf5File,
                gridDescription.geometry.shape = geometryObjectLocal->Shape(), gridDescription.geometry.fieldLocation = NODE, gridDescription.geometry.fieldType = VECTOR,
                gridDescription.geometry.componentDimension = gridDescription.geometry.shape.size() > 2 ? gridDescription.geometry.shape[2] : gridDescription.geometry.shape.back();
            } else {
                // grad the geometry from the particle_fields
                auto gridField = std::find_if(gridDescription.fields.begin(), gridDescription.fields.end(), [](const auto& f) { return f.name == "DMSwarmPIC_coor"; });
                if (gridField != gridDescription.fields.end()) {
                    gridDescription.geometry = *gridField;
                    gridDescription.fields.erase(gridField);
                } else {
                    throw std::runtime_error("Cannot determine geometry for particles");
                }
            }

            // hard code simple topology
            gridDescription.topology.location.path = "";
            gridDescription.topology.location.file = hdf5File;
            gridDescription.topology.number = gridDescription.geometry.GetDof();
            gridDescription.topology.numberCorners = 0;
            gridDescription.topology.dimension = gridDescription.geometry.GetDimension();

            particleGrid.grids[timeIndex].push_back(gridDescription);
        }

        // add to the list of grids
        specification->gridsCollections.push_back(particleGrid);
    }

    return specification;
}

std::shared_ptr<XdmfSpecification> xdmfGenerator::XdmfSpecification::FromPetscHdf(const std::function<std::shared_ptr<xdmfGenerator::HdfObject>()>& consumer) {
    // make a new specification
    auto specification = std::make_shared<XdmfSpecification>();

    // keep track of the time index
    std::size_t timeIndexCount = 0;

    // march over each object
    while (auto hdf5Object = consumer()) {
        // petsc hdf5 files may have a root domain (this is often a real mesh (FE/FV))
        std::shared_ptr<xdmfGenerator::HdfObject> geometryObject = FindPetscHdfChild(hdf5Object, "geometry");
        if (geometryObject) {
            // set up the grid collection for this specification
            auto& gridsCollections = specification->gridsCollections;
            if (gridsCollections.empty()) {
                gridsCollections.emplace_back();
            }
            auto& gridsCollection = gridsCollections.back();

            // march over each possible topology
            int topologyIndex = 0;
            while (auto topologyObject = FindPetscHdfChild(hdf5Object, "topology" + GetTopologyPostfix(topologyIndex))) {
                // store the file name
                auto hdf5File = hdf5Object->Name();

                // get the time
                auto time = hdf5Object->Contains("time") ? hdf5Object->Get("time")->RawData<double>() : std::vector<double>{-1};

                // add in each time
                GridDescription gridDescription;
                gridDescription.time = time[0];  // always the first time index

                // store the geometry
                auto verticesObject = geometryObject->Get("vertices");
                gridDescription.geometry.name = verticesObject->Name(), gridDescription.geometry.location.path = verticesObject->Path(), gridDescription.geometry.location.file = hdf5File,
                gridDescription.geometry.shape = verticesObject->Shape(), gridDescription.geometry.fieldLocation = NODE, gridDescription.geometry.fieldType = VECTOR,
                gridDescription.geometry.componentDimension = gridDescription.geometry.shape.size() > 2 ? gridDescription.geometry.shape[2] : gridDescription.geometry.shape.back();

                // check for and get the topology
                if (topologyObject) {
                    auto cellObject = topologyObject->Get("cells");
                    gridDescription.topology.location.path = cellObject->Path();
                    gridDescription.topology.location.file = hdf5File;
                    gridDescription.topology.number = cellObject->Shape()[0];
                    gridDescription.topology.numberCorners = cellObject->Shape()[1];
                    gridDescription.topology.dimension = cellObject->Attribute<unsigned long long>("cell_dim");
                }
                // hybrid topology
                std::shared_ptr<xdmfGenerator::HdfObject> hybridTopologyObject = FindPetscHdfChild(hdf5Object, "hybrid_topology");
                if (hybridTopologyObject) {
                    auto cellObject = topologyObject->Get("hcells");
                    gridDescription.hybridTopology.location.path = cellObject->Path();
                    gridDescription.hybridTopology.location.file = hdf5File;
                    gridDescription.hybridTopology.number = cellObject->Shape()[0];
                    gridDescription.hybridTopology.numberCorners = cellObject->Shape()[1];
                }

                // get the vertex fields and map into a vertex map. NOTE: multi file time index is always 0
                if (hdf5Object->Contains("vertex_fields")) {
                    GenerateFieldsFromPetsc(gridDescription.fields, hdf5Object->Get("vertex_fields")->Items(), NODE, hdf5File, 0);
                }
                if (hdf5Object->Contains("cell_fields")) {
                    GenerateFieldsFromPetsc(gridDescription.fields, hdf5Object->Get("cell_fields")->Items(), CELL, hdf5File, 0);
                }

                gridsCollection.grids[timeIndexCount].push_back(gridDescription);
                ++topologyIndex;
            }
        }
        // check for particles
        if (hdf5Object->Contains("particles") || hdf5Object->Contains("particle_fields")) {
            // set up the grid collection for this specification
            auto& gridsCollections = specification->gridsCollections;
            if (gridsCollections.empty()) {
                gridsCollections.emplace_back();
            }
            auto& gridsCollection = gridsCollections.back();
            gridsCollection.name = "particle_domain";

            // store the file name
            auto hdf5File = hdf5Object->Name();

            // get the time
            auto time = hdf5Object->Contains("time") ? hdf5Object->Get("time")->RawData<double>() : std::vector<double>{-1};

            for (double timeIndex : time) {
                GridDescription gridDescription;
                gridDescription.time = timeIndex;

                // add in any other fields. NOTE: time offset for multi file is always zero
                if (hdf5Object->Contains("particle_fields")) {
                    GenerateFieldsFromPetsc(gridDescription.fields, hdf5Object->Get("particle_fields")->Items(), NODE, hdf5File, 0);
                }

                if (hdf5Object->Contains("particles")) {
                    std::shared_ptr<xdmfGenerator::HdfObject> geometryObjectLocal = hdf5Object->Get("particles")->Get("coordinates");
                    // store the geometry
                    gridDescription.geometry.name = geometryObjectLocal->Name(), gridDescription.geometry.location.path = geometryObjectLocal->Path(),
                    gridDescription.geometry.location.file = hdf5File, gridDescription.geometry.shape = geometryObjectLocal->Shape(), gridDescription.geometry.fieldLocation = NODE,
                    gridDescription.geometry.fieldType = VECTOR,
                    gridDescription.geometry.componentDimension = gridDescription.geometry.shape.size() > 2 ? gridDescription.geometry.shape[2] : gridDescription.geometry.shape.back();
                } else {
                    // grad the geometry from the particle_fields
                    auto gridField = std::find_if(gridDescription.fields.begin(), gridDescription.fields.end(), [](const auto& f) { return f.name == "DMSwarmPIC_coor"; });
                    if (gridField != gridDescription.fields.end()) {
                        gridDescription.geometry = *gridField;
                        gridDescription.fields.erase(gridField);
                    } else {
                        throw std::runtime_error("Cannot determine geometry for particles");
                    }
                }

                // hard code simple topology
                gridDescription.topology.location.path = "";
                gridDescription.topology.location.file = hdf5File;
                gridDescription.topology.number = gridDescription.geometry.GetDof();
                gridDescription.topology.numberCorners = 0;
                gridDescription.topology.dimension = gridDescription.geometry.GetDimension();

                gridsCollection.grids[timeIndexCount].push_back(gridDescription);
            }
        }
        ++timeIndexCount;
    }

    return specification;
}

std::shared_ptr<xdmfGenerator::HdfObject> XdmfSpecification::FindPetscHdfChild(std::shared_ptr<xdmfGenerator::HdfObject>& root, const std::string& name) {
    if (root->Contains("viz") && root->Get("viz")->Contains(name)) {
        return root->Get("viz")->Get(name);
    } else if (root->Contains(name)) {
        return root->Get(name);
    }

    return nullptr;
}
