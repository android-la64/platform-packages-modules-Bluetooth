use crate::{
    gatt::{
        ids::AttHandle,
        server::{
            att_database::{AttAttribute, AttDatabase, StableAttDatabase},
            gatt_database::AttPermissions,
        },
    },
    packets::{AttAttributeDataChild, AttErrorCode},
};

use async_trait::async_trait;
use log::info;
use std::collections::BTreeMap;

pub struct TestAttDatabase {
    attributes: BTreeMap<AttHandle, (AttAttribute, Vec<u8>)>,
}

impl TestAttDatabase {
    #[cfg(test)]
    pub fn new(attributes: Vec<(AttAttribute, Vec<u8>)>) -> Self {
        Self {
            attributes: attributes
                .into_iter()
                .map(|(att, data)| (att.handle, (att, data)))
                .collect(),
        }
    }
}

#[async_trait(?Send)]
impl AttDatabase for TestAttDatabase {
    async fn read_attribute(
        &self,
        handle: AttHandle,
    ) -> Result<AttAttributeDataChild, AttErrorCode> {
        info!("reading {handle:?}");
        match self.attributes.get(&handle) {
            Some((AttAttribute { permissions: AttPermissions { readable: false, .. }, .. }, _)) => {
                Err(AttErrorCode::READ_NOT_PERMITTED)
            }
            Some((_, data)) => Ok(AttAttributeDataChild::RawData(data.clone().into_boxed_slice())),
            None => Err(AttErrorCode::INVALID_HANDLE),
        }
    }
    fn list_attributes(&self) -> Vec<AttAttribute> {
        self.attributes.values().map(|(att, _)| *att).collect()
    }
}

// We guarantee that the contents of a TestAttDatabase will remain stable
impl StableAttDatabase for TestAttDatabase {}
