import { useState, useRef } from 'react';
import { Button, Form, Alert } from 'react-bootstrap';
import { useTranslation } from 'react-i18next';
import Section from '../Components/Section';
import WebApi from '../Services/WebApi';

export default function VMUManagerPage() {
	const { t } = useTranslation('');

	const [exportLoading, setExportLoading] = useState(false);
	const [exportError, setExportError] = useState('');

	const [importLoading, setImportLoading] = useState(false);
	const [importSuccess, setImportSuccess] = useState(false);
	const [importError, setImportError] = useState('');
	const importFileRef = useRef();

	const [formatConfirm, setFormatConfirm] = useState('');
	const [formatLoading, setFormatLoading] = useState(false);
	const [formatSuccess, setFormatSuccess] = useState(false);
	const [formatError, setFormatError] = useState('');

	const handleExport = async () => {
		setExportLoading(true);
		setExportError('');
		try {
			const buffer = await WebApi.getVMUData();
			const blob = new Blob([buffer], { type: 'application/octet-stream' });
			const url = URL.createObjectURL(blob);
			const a = document.createElement('a');
			a.href = url;
			a.download = 'vmu_backup.bin';
			a.click();
			URL.revokeObjectURL(url);
		} catch (e) {
			setExportError(e.message);
		}
		setExportLoading(false);
	};

	const handleImport = async (e) => {
		const file = e.target.files[0];
		if (!file) return;
		setImportLoading(true);
		setImportSuccess(false);
		setImportError('');
		try {
			const buffer = await file.arrayBuffer();
			const response = await WebApi.importVMUSave(buffer);
			if (!response.success) throw new Error(response.error || 'Import failed');
			setImportSuccess(true);
		} catch (e) {
			setImportError(e.message);
		}
		setImportLoading(false);
		if (importFileRef.current) importFileRef.current.value = '';
	};

	const handleFormat = async () => {
		if (formatConfirm !== 'FORMAT') return;
		setFormatLoading(true);
		setFormatSuccess(false);
		setFormatError('');
		try {
			const response = await WebApi.formatVMU();
			if (!response.success) throw new Error(response.error || 'Format failed');
			setFormatSuccess(true);
			setFormatConfirm('');
		} catch (e) {
			setFormatError(e.message);
		}
		setFormatLoading(false);
	};

	return (
		<div>
			<Section title={t('VMUManagerPage:export-header')}>
				<p>{t('VMUManagerPage:export-description')}</p>
				{exportError && (
					<Alert variant="danger">
						{t('VMUManagerPage:error-prefix')}{exportError}
					</Alert>
				)}
				<Button
					variant="primary"
					onClick={handleExport}
					disabled={exportLoading}
				>
					{exportLoading
						? t('VMUManagerPage:export-loading')
						: t('VMUManagerPage:export-button')}
				</Button>
			</Section>

			<Section title={t('VMUManagerPage:import-header')}>
				<p>{t('VMUManagerPage:import-description')}</p>
				{importSuccess && (
					<Alert variant="success">{t('VMUManagerPage:import-success')}</Alert>
				)}
				{importError && (
					<Alert variant="danger">
						{t('VMUManagerPage:error-prefix')}{importError}
					</Alert>
				)}
				<Form.Control
					ref={importFileRef}
					type="file"
					accept=".dci"
					disabled={importLoading}
					onChange={handleImport}
				/>
				{importLoading && (
					<p className="mt-2">{t('VMUManagerPage:import-loading')}</p>
				)}
			</Section>

			<Section title={t('VMUManagerPage:format-header')}>
				<p className="text-danger fw-bold">{t('VMUManagerPage:format-description')}</p>
				{formatSuccess && (
					<Alert variant="success">{t('VMUManagerPage:format-success')}</Alert>
				)}
				{formatError && (
					<Alert variant="danger">
						{t('VMUManagerPage:error-prefix')}{formatError}
					</Alert>
				)}
				<Form.Group className="mb-2">
					<Form.Label>{t('VMUManagerPage:format-confirm-label')}</Form.Label>
					<Form.Control
						type="text"
						placeholder={t('VMUManagerPage:format-confirm-placeholder')}
						value={formatConfirm}
						onChange={(e) => setFormatConfirm(e.target.value)}
						disabled={formatLoading}
					/>
				</Form.Group>
				<Button
					variant="danger"
					onClick={handleFormat}
					disabled={formatLoading || formatConfirm !== 'FORMAT'}
				>
					{formatLoading
						? t('VMUManagerPage:format-loading')
						: t('VMUManagerPage:format-button')}
				</Button>
			</Section>
		</div>
	);
}
